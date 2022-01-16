#pragma once

#include "util/types.hpp"
#include "util/v128.hpp"
#include "util/sysinfo.hpp"
#include "Utilities/JIT.h"

#if defined(ARCH_X64)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include <immintrin.h>
#include <emmintrin.h>
#include <cmath>
#endif

#if defined(ARCH_ARM64)
#include <arm_neon.h>
#endif

#include <cmath>
#include <cfenv>

namespace asmjit
{
	struct vec_builder;
}

inline thread_local asmjit::vec_builder* g_vc = nullptr;

namespace asmjit
{
#if defined(ARCH_X64)
	using gpr_type = x86::Gp;
	using vec_type = x86::Xmm;
	using mem_type = x86::Mem;
#else
	struct gpr_type : Operand
	{
		gpr_type(u32)
		{
		}
	};

	struct vec_type : Operand
	{
		vec_type(u32)
		{
		}
	};

	struct mem_type : Operand
	{
	};
#endif

	struct mem_lazy : Operand
	{
		const Operand& eval(bool is_lv);
	};

	enum class arg_class : u32
	{
		reg_lv, // const auto x = gv_...(y, z);
		reg_rv, // r = gv_...(y, z);
		imm_lv,
		imm_rv,
		mem_lv,
		mem_rv,
	};

	constexpr arg_class operator+(arg_class _base, u32 off)
	{
		return arg_class(u32(_base) + off);
	}

	template <typename... Args>
	constexpr bool any_operand_v = (std::is_base_of_v<Operand, std::decay_t<Args>> || ...);

	template <typename T, typename D = std::decay_t<T>>
	constexpr arg_class arg_classify =
		std::is_base_of_v<v128, D> ? arg_class::imm_lv + !std::is_reference_v<T> :
		std::is_base_of_v<mem_type, D> ? arg_class::mem_lv :
		std::is_base_of_v<mem_lazy, D> ? arg_class::mem_lv + !std::is_reference_v<T> :
		std::is_reference_v<T> ? arg_class::reg_lv : arg_class::reg_rv;

	struct vec_builder : native_asm
	{
		using base = native_asm;

		vec_builder(CodeHolder* ch)
			: native_asm(ch)
		{
			if (!g_vc)
			{
				g_vc = this;
			}
		}

		~vec_builder()
		{
			if (g_vc == this)
			{
				g_vc = nullptr;
			}
		}

		u32 vec_allocated = 0xffffffff << 6;

		vec_type vec_alloc()
		{
			ensure(~vec_allocated);
			const u32 idx = std::countr_one(vec_allocated);
			vec_allocated |= vec_allocated + 1;
			return vec_type{idx};
		}

		template <u32 Size>
		std::array<vec_type, Size> vec_alloc()
		{
			std::array<vec_type, Size> r;
			for (auto& x : r)
			{
				x = vec_alloc();
			}
			return r;
		}

		void vec_dealloc(vec_type vec)
		{
			vec_allocated &= ~(1u << vec.id());
		}

		void emit_consts()
		{
			//  (TODO: sort in use order)
			for (u32 sz = 1; sz <= 16; sz++)
			{
				for (auto& [key, _label] : consts[sz - 1])
				{
					base::align(AlignMode::kData, 1u << std::countr_zero<u32>(sz));
					base::bind(_label);
					base::embed(&key, sz);
				}
			}
		}

		std::unordered_map<v128, Label> consts[16]{};

		template <typename T, u32 Size = sizeof(T)>
		x86::Mem get_const(const T& data, u32 esize = Size)
		{
			static_assert(Size <= 16);

			// Find existing const
			v128 key{};
			std::memcpy(&key, &data, Size);

			if (Size == 16 && esize == 4 && key._u64[0] == key._u64[1] && key._u32[0] == key._u32[1])
			{
				x86::Mem r = get_const<u32>(key._u32[0]);
				r.setBroadcast(x86::Mem::Broadcast::k1To4);
				return r;
			}

			if (Size == 16 && esize == 8 && key._u64[0] == key._u64[1])
			{
				x86::Mem r = get_const<u64>(key._u64[0]);
				r.setBroadcast(x86::Mem::Broadcast::k1To2);
				return r;
			}

			auto& _label = consts[Size - 1][key];

			if (!_label.isValid())
				_label = base::newLabel();

			return x86::Mem(_label, 0, Size);
		}
	};

#if defined(ARCH_X64)
	inline auto arg_eval(const v128& _c, u32 esize)
	{
		// TODO: implement PSHUFD broadcasts and AVX ones
		auto r = g_vc->get_const(_c, esize);
		return r;
	}

	template <typename T> requires(std::is_base_of_v<mem_lazy, std::decay_t<T>>)
	inline decltype(auto) arg_eval(T&& mem, u32)
	{
		return mem.eval(std::is_reference_v<T>);
	}

	inline decltype(auto) arg_eval(Operand& mem, u32)
	{
		return mem;
	}

	inline decltype(auto) arg_eval(Operand&& mem, u32)
	{
		return std::move(mem);
	}

	template <typename A, typename... Args>
	vec_type unary_op(x86::Inst::Id op, x86::Inst::Id op2, A&& a, Args&&... args)
	{
		if constexpr (arg_classify<A> == arg_class::reg_rv)
		{
			if (op)
			{
				ensure(!g_vc->emit(op, a, std::forward<Args>(args)...));
			}
			else
			{
				ensure(!g_vc->emit(op2, a, a, std::forward<Args>(args)...));
			}

			return a;
		}
		else
		{
			vec_type r = g_vc->vec_alloc();

			if (op)
			{
				if (op2 && utils::has_avx())
				{
					// Assume op2 is AVX (but could be PSHUFD as well for example)
					ensure(!g_vc->emit(op2, r, arg_eval(std::forward<A>(a), 16), std::forward<Args>(args)...));
				}
				else
				{
					// TODO
					ensure(!g_vc->emit(x86::Inst::Id::kIdMovaps, r, arg_eval(std::forward<A>(a), 16)));
					ensure(!g_vc->emit(op, r, std::forward<Args>(args)...));
				}
			}
			else
			{
				ensure(!g_vc->emit(op2, r, arg_eval(std::forward<A>(a), 16), std::forward<Args>(args)...));
			}

			return r;
		}
	}

	template <typename D, typename S>
	void store_op(x86::Inst::Id op, x86::Inst::Id evex_op, D&& d, S&& s)
	{
		static_assert(arg_classify<D> == arg_class::mem_lv);

		mem_type dst;
		dst.copyFrom(arg_eval(std::forward<D>(d), 16));

		if (utils::has_avx512() && evex_op)
		{
			if (!dst.hasBaseLabel() && dst.hasOffset() && dst.offset() % dst.size() == 0 && dst.offset() / dst.size() + 128 < 256)
			{
				ensure(!g_vc->evex().emit(evex_op, dst, arg_eval(std::forward<S>(s), 16)));
				return;
			}
		}

		ensure(!g_vc->emit(op, dst, arg_eval(std::forward<S>(s), 16)));
	}

	template <typename A, typename B, typename... Args>
	vec_type binary_op(u32 esize, x86::Inst::Id mov_op, x86::Inst::Id sse_op, x86::Inst::Id avx_op, x86::Inst::Id evex_op, A&& a, B&& b, Args&&... args)
	{
		Operand src1{};

		if constexpr (arg_classify<A> == arg_class::reg_rv)
		{
			// Use src1 as a destination
			src1 = arg_eval(std::forward<A>(a), 16);

			if (utils::has_avx512() && evex_op && (arg_classify<B> == arg_class::imm_rv || arg_classify<B> == arg_class::mem_rv || b.isMem()))
			{
				ensure(!g_vc->evex().emit(evex_op, src1, src1, arg_eval(std::forward<B>(b), esize), std::forward<Args>(args)...));
				return vec_type{src1.id()};
			}

			if constexpr (arg_classify<B> == arg_class::reg_rv)
			{
				g_vc->vec_dealloc(vec_type{b.id()});
			}
		}
		else if (utils::has_avx() && avx_op && (arg_classify<A> == arg_class::reg_lv || arg_classify<A> == arg_class::mem_lv))
		{
			if constexpr (arg_classify<A> == arg_class::reg_lv)
			{
				if constexpr (arg_classify<B> == arg_class::reg_rv)
				{
					// Use src2 as a destination
					src1 = arg_eval(std::forward<B>(b), 16);
				}
				else
				{
					// Use new reg as a destination
					src1 = g_vc->vec_alloc();
				}
			}
			else // if A == arg_class::reg_rv
			{
				src1 = g_vc->vec_alloc();

				if (!a.isReg())
				{
					static_cast<void>(arg_eval(std::forward<A>(a), 16));
				}

				if constexpr (arg_classify<B> == arg_class::reg_rv)
				{
					g_vc->vec_dealloc(vec_type{b.id()});
				}
			}

			if (utils::has_avx512() && evex_op && (arg_classify<B> == arg_class::imm_rv || arg_classify<B> == arg_class::mem_rv || b.isMem()))
			{
				ensure(!g_vc->evex().emit(evex_op, src1, vec_type{a.id()}, arg_eval(std::forward<B>(b), esize), std::forward<Args>(args)...));
				return vec_type{src1.id()};
			}

			ensure(!g_vc->emit(avx_op, src1, vec_type{a.id()}, arg_eval(std::forward<B>(b), 16), std::forward<Args>(args)...));
			return vec_type{src1.id()};
		}
		else do
		{
			if constexpr (arg_classify<B> == arg_class::reg_rv)
			{
				g_vc->vec_dealloc(vec_type{b.id()});
			}

			if (arg_classify<A> == arg_class::mem_rv && a.isReg())
			{
				src1 = vec_type(a.id());
				break;
			}

			src1 = g_vc->vec_alloc();

			// Fallback to arg copy
			ensure(!g_vc->emit(mov_op, src1, arg_eval(std::forward<A>(a), 16)));
		}
		while (0);

		if (utils::has_avx512() && evex_op && (arg_classify<B> == arg_class::imm_rv || arg_classify<B> == arg_class::mem_rv || b.isMem()))
		{
			ensure(!g_vc->evex().emit(evex_op, src1, src1, arg_eval(std::forward<B>(b), esize), std::forward<Args>(args)...));
		}
		else if (sse_op)
		{
			ensure(!g_vc->emit(sse_op, src1, arg_eval(std::forward<B>(b), 16), std::forward<Args>(args)...));
		}
		else
		{
			ensure(!g_vc->emit(avx_op, src1, src1, arg_eval(std::forward<B>(b), 16), std::forward<Args>(args)...));
		}

		return vec_type{src1.id()};
	}
#define FOR_X64(f, ...) do { using enum asmjit::x86::Inst::Id; return asmjit::f(__VA_ARGS__); } while (0)
#elif defined(ARCH_ARM64)
#define FOR_X64(...) do {} while (0)
#endif
}

inline v128 gv_select8(const v128& _cmp, const v128& _true, const v128& _false);
inline v128 gv_select16(const v128& _cmp, const v128& _true, const v128& _false);
inline v128 gv_select32(const v128& _cmp, const v128& _true, const v128& _false);
inline v128 gv_selectfs(const v128& _cmp, const v128& _true, const v128& _false);

inline void gv_set_zeroing_denormals()
{
#if defined(ARCH_X64)
	u32 cr = _mm_getcsr();
	cr = (cr & ~_MM_FLUSH_ZERO_MASK) | _MM_FLUSH_ZERO_ON;
	cr = (cr & ~_MM_DENORMALS_ZERO_MASK) | _MM_DENORMALS_ZERO_ON;
	cr = (cr | _MM_MASK_INVALID);
	_mm_setcsr(cr);
#elif defined(ARCH_ARM64)
	u64 cr;
	__asm__ volatile("mrs %0, FPCR" : "=r"(cr));
	cr |= 0x1000000ull;
	__asm__ volatile("msr FPCR, %0" :: "r"(cr));
#else
#error "Not implemented"
#endif
}

inline void gv_unset_zeroing_denormals()
{
#if defined(ARCH_X64)
	u32 cr = _mm_getcsr();
	cr = (cr & ~_MM_FLUSH_ZERO_MASK) | _MM_FLUSH_ZERO_OFF;
	cr = (cr & ~_MM_DENORMALS_ZERO_MASK) | _MM_DENORMALS_ZERO_OFF;
	cr = (cr | _MM_MASK_INVALID);
	_mm_setcsr(cr);
#elif defined(ARCH_ARM64)
	u64 cr;
	__asm__ volatile("mrs %0, FPCR" : "=r"(cr));
	cr &= ~0x1000000ull;
	__asm__ volatile("msr FPCR, %0" :: "r"(cr));
#else
#error "Not implemented"
#endif
}

inline void gv_zeroupper()
{
#if defined(ARCH_X64)
	if (!utils::has_avx())
		return;
#if defined(_M_X64)
	_mm256_zeroupper();
#else
	__asm__ volatile("vzeroupper;");
#endif
#endif
}

inline v128 gv_bcst8(u8 value)
{
#if defined(ARCH_X64)
	return _mm_set1_epi8(value);
#elif defined(ARCH_ARM64)
	return vdupq_n_s8(value);
#endif
}

inline v128 gv_bcst16(u16 value)
{
#if defined(ARCH_X64)
	return _mm_set1_epi16(value);
#elif defined(ARCH_ARM64)
	return vdupq_n_s16(value);
#endif
}

// Optimized broadcast using constant offset assumption
inline v128 gv_bcst16(const u16& value, auto mptr, auto... args)
{
#if defined(ARCH_X64)
	const u32 offset = ::offset32(mptr, args...);
	[[maybe_unused]] const __m128i* ptr = reinterpret_cast<__m128i*>(uptr(&value) - offset % 16);
#if !defined(__AVX2__)
	if (offset % 16 == 0)
		return _mm_shuffle_epi32(_mm_shufflelo_epi16(*ptr, 0), 0);
	if (offset % 16 == 2)
		return _mm_shuffle_epi32(_mm_shufflelo_epi16(*ptr, 0b01010101), 0);
	if (offset % 16 == 4)
		return _mm_shuffle_epi32(_mm_shufflelo_epi16(*ptr, 0b10101010), 0);
	if (offset % 16 == 6)
		return _mm_shuffle_epi32(_mm_shufflelo_epi16(*ptr, 0xff), 0);
	if (offset % 16 == 8)
		return _mm_shuffle_epi32(_mm_shufflehi_epi16(*ptr, 0), 0xff);
	if (offset % 16 == 10)
		return _mm_shuffle_epi32(_mm_shufflehi_epi16(*ptr, 0b01010101), 0xff);
	if (offset % 16 == 12)
		return _mm_shuffle_epi32(_mm_shufflehi_epi16(*ptr, 0b10101010), 0xff);
	if (offset % 16 == 14)
		return _mm_shuffle_epi32(_mm_shufflehi_epi16(*ptr, 0xff), 0xff);
#endif
	return _mm_set1_epi16(value);
#else
	static_cast<void>(mptr);
	return gv_bcst16(value);
#endif
}

inline v128 gv_bcst32(u32 value)
{
#if defined(ARCH_X64)
	return _mm_set1_epi32(value);
#elif defined(ARCH_ARM64)
	return vdupq_n_s32(value);
#endif
}

// Optimized broadcast using constant offset assumption
inline v128 gv_bcst32(const u32& value, auto mptr, auto... args)
{
#if defined(ARCH_X64)
	const u32 offset = ::offset32(mptr, args...);
	[[maybe_unused]] const __m128i* ptr = reinterpret_cast<__m128i*>(uptr(&value) - offset % 16);
#if !defined(__AVX__)
	if (offset % 16 == 0)
		return _mm_shuffle_epi32(*ptr, 0);
	if (offset % 16 == 4)
		return _mm_shuffle_epi32(*ptr, 0b01010101);
	if (offset % 16 == 8)
		return _mm_shuffle_epi32(*ptr, 0b10101010);
	if (offset % 16 == 12)
		return _mm_shuffle_epi32(*ptr, 0xff);
#endif
	return _mm_set1_epi32(value);
#else
	static_cast<void>(mptr);
	return gv_bcst32(value);
#endif
}

inline v128 gv_bcst64(u64 value)
{
#if defined(ARCH_X64)
	return _mm_set1_epi64x(value);
#elif defined(ARCH_ARM64)
	return vdupq_n_s64(value);
#endif
}

// Optimized broadcast using constant offset assumption
inline v128 gv_bcst64(const u64& value, auto mptr, auto... args)
{
#if defined(ARCH_X64)
	const u32 offset = ::offset32(mptr, args...);
	[[maybe_unused]] const __m128i* ptr = reinterpret_cast<__m128i*>(uptr(&value) - offset % 16);
#if !defined(__AVX__)
	if (offset % 16 == 0)
		return _mm_shuffle_epi32(*ptr, 0b00010001);
	if (offset % 16 == 8)
		return _mm_shuffle_epi32(*ptr, 0b10111011);
#endif
	return _mm_set1_epi64x(value);
#else
	static_cast<void>(mptr);
	return gv_bcst64(value);
#endif
}

inline v128 gv_bcstfs(f32 value)
{
#if defined(ARCH_X64)
	return _mm_set1_ps(value);
#elif defined(ARCH_ARM64)
	return vdupq_n_f32(value);
#endif
}

inline v128 gv_and32(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_and_si128(a, b);
#elif defined(ARCH_ARM64)
	return vandq_s32(a, b);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_and32(A&& a, B&& b)
{
	FOR_X64(binary_op, 4, kIdMovdqa, kIdPand, kIdVpand, kIdVpandd, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_andfs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_and_ps(a, b);
#elif defined(ARCH_ARM64)
	return vandq_s32(a, b);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_andfs(A&& a, B&& b)
{
	FOR_X64(binary_op, 4, kIdMovaps, kIdAndps, kIdVandps, kIdVandps, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_andn32(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_andnot_si128(a, b);
#elif defined(ARCH_ARM64)
	return vbicq_s32(b, a);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_andn32(A&& a, B&& b)
{
	FOR_X64(binary_op, 4, kIdMovdqa, kIdPandn, kIdVpandn, kIdVpandnd, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_andnfs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_andnot_ps(a, b);
#elif defined(ARCH_ARM64)
	return vbicq_s32(b, a);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_andnfs(A&& a, B&& b)
{
	FOR_X64(binary_op, 4, kIdMovaps, kIdAndnps, kIdVandnps, kIdVandnps, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_or32(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_or_si128(a, b);
#elif defined(ARCH_ARM64)
	return vorrq_s32(a, b);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_or32(A&& a, B&& b)
{
	FOR_X64(binary_op, 4, kIdMovdqa, kIdPor, kIdVpor, kIdVpord, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_orfs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_or_ps(a, b);
#elif defined(ARCH_ARM64)
	return vorrq_s32(a, b);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_orfs(A&& a, B&& b)
{
	FOR_X64(binary_op, 4, kIdMovaps, kIdOrps, kIdVorps, kIdVorps, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_xor32(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_xor_si128(a, b);
#elif defined(ARCH_ARM64)
	return veorq_s32(a, b);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_xor32(A&& a, B&& b)
{
	FOR_X64(binary_op, 4, kIdMovdqa, kIdPxor, kIdVpxor, kIdVpxord, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_xorfs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_xor_ps(a, b);
#elif defined(ARCH_ARM64)
	return veorq_s32(a, b);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_xorfs(A&& a, B&& b)
{
	FOR_X64(binary_op, 4, kIdMovaps, kIdXorps, kIdVxorps, kIdVxorps, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_not32(const v128& a)
{
#if defined(ARCH_X64)
	return _mm_xor_si128(a, _mm_set1_epi32(-1));
#elif defined(ARCH_ARM64)
	return vmvnq_u32(a);
#endif
}

inline v128 gv_notfs(const v128& a)
{
#if defined(ARCH_X64)
	return _mm_xor_ps(a, _mm_castsi128_ps(_mm_set1_epi32(-1)));
#elif defined(ARCH_ARM64)
	return vmvnq_u32(a);
#endif
}

inline v128 gv_shl16(const v128& a, u32 count)
{
	if (count >= 16)
		return v128{};
#if defined(ARCH_X64)
	return _mm_slli_epi16(a, count);
#elif defined(ARCH_ARM64)
	return vshlq_s16(a, vdupq_n_s16(count));
#endif
}

template <typename A> requires(asmjit::any_operand_v<A>)
inline auto gv_shl16(A&& a, u32 count)
{
	FOR_X64(unary_op, kIdPsllw, kIdVpsllw, std::forward<A>(a), count);
}

inline v128 gv_shl32(const v128& a, u32 count)
{
	if (count >= 32)
		return v128{};
#if defined(ARCH_X64)
	return _mm_slli_epi32(a, count);
#elif defined(ARCH_ARM64)
	return vshlq_s32(a, vdupq_n_s32(count));
#endif
}

template <typename A> requires(asmjit::any_operand_v<A>)
inline auto gv_shl32(A&& a, u32 count)
{
	FOR_X64(unary_op, kIdPslld, kIdVpslld, std::forward<A>(a), count);
}

inline v128 gv_shl64(const v128& a, u32 count)
{
	if (count >= 64)
		return v128{};
#if defined(ARCH_X64)
	return _mm_slli_epi64(a, count);
#elif defined(ARCH_ARM64)
	return vshlq_s64(a, vdupq_n_s64(count));
#endif
}

template <typename A> requires(asmjit::any_operand_v<A>)
inline auto gv_shl64(A&& a, u32 count)
{
	FOR_X64(unary_op, kIdPsllq, kIdVpsllq, std::forward<A>(a), count);
}

inline v128 gv_shr16(const v128& a, u32 count)
{
	if (count >= 16)
		return v128{};
#if defined(ARCH_X64)
	return _mm_srli_epi16(a, count);
#elif defined(ARCH_ARM64)
	return vshlq_u16(a, vdupq_n_s16(0 - count));
#endif
}

template <typename A> requires(asmjit::any_operand_v<A>)
inline auto gv_shr16(A&& a, u32 count)
{
	FOR_X64(unary_op, kIdPsrlw, kIdVpsrlw, std::forward<A>(a), count);
}

inline v128 gv_shr32(const v128& a, u32 count)
{
	if (count >= 32)
		return v128{};
#if defined(ARCH_X64)
	return _mm_srli_epi32(a, count);
#elif defined(ARCH_ARM64)
	return vshlq_u32(a, vdupq_n_s32(0 - count));
#endif
}

template <typename A> requires(asmjit::any_operand_v<A>)
inline auto gv_shr32(A&& a, u32 count)
{
	FOR_X64(unary_op, kIdPsrld, kIdVpsrld, std::forward<A>(a), count);
}

inline v128 gv_shr64(const v128& a, u32 count)
{
	if (count >= 64)
		return v128{};
#if defined(ARCH_X64)
	return _mm_srli_epi64(a, count);
#elif defined(ARCH_ARM64)
	return vshlq_u64(a, vdupq_n_s64(0 - count));
#endif
}

template <typename A> requires(asmjit::any_operand_v<A>)
inline auto gv_shr64(A&& a, u32 count)
{
	FOR_X64(unary_op, kIdPsrlq, kIdVpsrlq, std::forward<A>(a), count);
}

inline v128 gv_sar16(const v128& a, u32 count)
{
	if (count >= 16)
		count = 15;
#if defined(ARCH_X64)
	return _mm_srai_epi16(a, count);
#elif defined(ARCH_ARM64)
	return vshlq_s16(a, vdupq_n_s16(0 - count));
#endif
}

template <typename A> requires(asmjit::any_operand_v<A>)
inline auto gv_sar16(A&& a, u32 count)
{
	FOR_X64(unary_op, kIdPsraw, kIdVpsraw, std::forward<A>(a), count);
}

inline v128 gv_sar32(const v128& a, u32 count)
{
	if (count >= 32)
		count = 31;
#if defined(ARCH_X64)
	return _mm_srai_epi32(a, count);
#elif defined(ARCH_ARM64)
	return vshlq_s32(a, vdupq_n_s32(0 - count));
#endif
}

template <typename A> requires(asmjit::any_operand_v<A>)
inline auto gv_sar32(A&& a, u32 count)
{
	FOR_X64(unary_op, kIdPsrad, kIdVpsrad, std::forward<A>(a), count);
}

inline v128 gv_sar64(const v128& a, u32 count)
{
	if (count >= 64)
		count = 63;
#if defined(__AVX512VL__)
	return _mm_srai_epi64(a, count);
#elif defined(__SSE2__) && !defined(_M_X64)
	return static_cast<__v2di>(a) >> count;
#elif defined(ARCH_ARM64)
	return vshlq_s64(a, vdupq_n_s64(0 - count));
#else
	v128 r;
	r._s64[0] = a._s64[0] >> count;
	r._s64[1] = a._s64[1] >> count;
	return r;
#endif
}

inline v128 gv_add8(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_add_epi8(a, b);
#elif defined(ARCH_ARM64)
	return vaddq_s8(a, b);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_add8(A&& a, B&& b)
{
	FOR_X64(binary_op, 1, kIdMovdqa, kIdPaddb, kIdVpaddb, kIdNone, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_add16(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_add_epi16(a, b);
#elif defined(ARCH_ARM64)
	return vaddq_s16(a, b);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_add16(A&& a, B&& b)
{
	FOR_X64(binary_op, 2, kIdMovdqa, kIdPaddw, kIdVpaddw, kIdNone, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_add32(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_add_epi32(a, b);
#elif defined(ARCH_ARM64)
	return vaddq_s32(a, b);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_add32(A&& a, B&& b)
{
	FOR_X64(binary_op, 4, kIdMovdqa, kIdPaddd, kIdVpaddd, kIdVpaddd, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_add64(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_add_epi64(a, b);
#elif defined(ARCH_ARM64)
	return vaddq_s64(a, b);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_add64(A&& a, B&& b)
{
	FOR_X64(binary_op, 8, kIdMovdqa, kIdPaddq, kIdVpaddq, kIdVpaddq, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_adds_s8(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_adds_epi8(a, b);
#elif defined(ARCH_ARM64)
	return vqaddq_s8(a, b);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_adds_s8(A&& a, B&& b)
{
	FOR_X64(binary_op, 1, kIdMovdqa, kIdPaddsb, kIdVpaddsb, kIdNone, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_adds_s16(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_adds_epi16(a, b);
#elif defined(ARCH_ARM64)
	return vqaddq_s16(a, b);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_adds_s16(A&& a, B&& b)
{
	FOR_X64(binary_op, 2, kIdMovdqa, kIdPaddsw, kIdVpaddsw, kIdNone, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_adds_s32(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	const v128 s = _mm_add_epi32(a, b);
	const v128 m = (a ^ s) & (b ^ s); // overflow bit
	const v128 x = _mm_srai_epi32(m, 31); // saturation mask
	const v128 y = _mm_srai_epi32(_mm_and_si128(s, m), 31); // positive saturation mask
	return _mm_xor_si128(_mm_xor_si128(_mm_srli_epi32(x, 1), y), _mm_or_si128(s, x));
#elif defined(ARCH_ARM64)
	return vqaddq_s32(a, b);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_adds_s32(A&& a, B&& b)
{
#if defined(ARCH_X64)
	auto s = gv_add32(a, b);
	auto m = gv_and32(gv_xor32(std::forward<A>(a), s), gv_xor32(std::forward<B>(b), s));
	auto x = gv_sar32(m, 31);
	auto y = gv_sar32(gv_and32(s, std::move(m)), 31);
	auto z = gv_xor32(gv_shr32(x, 1), std::move(y));
	return gv_xor32(std::move(z), gv_or32(std::move(s), std::move(x)));
#endif
}

inline v128 gv_addus_u8(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_adds_epu8(a, b);
#elif defined(ARCH_ARM64)
	return vqaddq_u8(a, b);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_addus_u8(A&& a, B&& b)
{
	FOR_X64(binary_op, 1, kIdMovdqa, kIdPaddusb, kIdVpaddusb, kIdNone, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_addus_u16(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_adds_epu16(a, b);
#elif defined(ARCH_ARM64)
	return vqaddq_u16(a, b);
#endif
}

template <typename A, typename B> requires (asmjit::any_operand_v<A, B>)
inline auto gv_addus_u16(A&& a, B&& b)
{
	FOR_X64(binary_op, 2, kIdMovdqa, kIdPaddusw, kIdVpaddusw, kIdNone, std::forward<A>(a), std::forward<B>(b));
}

inline v128 gv_addus_u32(const v128& a, const v128& b)
{
#if defined(__SSE4_1__)
	return _mm_add_epi32(a, _mm_min_epu32(~a, b));
#elif defined(ARCH_X64)
	const v128 s = _mm_add_epi32(a, b);
	return _mm_or_si128(s, _mm_cmpgt_epi32(_mm_xor_si128(b, _mm_set1_epi32(smin)), _mm_xor_si128(a, _mm_set1_epi32(smax))));
#elif defined(ARCH_ARM64)
	return vqaddq_u32(a, b);
#endif
}

inline v128 gv_addfs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_add_ps(a, b);
#elif defined(ARCH_ARM64)
	return vaddq_f32(a, b);
#endif
}

inline v128 gv_addfd(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_add_pd(a, b);
#elif defined(ARCH_ARM64)
	return vaddq_f64(a, b);
#endif
}

inline v128 gv_sub8(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_sub_epi8(a, b);
#elif defined(ARCH_ARM64)
	return vsubq_s8(a, b);
#endif
}

inline v128 gv_sub16(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_sub_epi16(a, b);
#elif defined(ARCH_ARM64)
	return vsubq_s16(a, b);
#endif
}

inline v128 gv_sub32(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_sub_epi32(a, b);
#elif defined(ARCH_ARM64)
	return vsubq_s32(a, b);
#endif
}

inline v128 gv_sub64(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_sub_epi64(a, b);
#elif defined(ARCH_ARM64)
	return vsubq_s64(a, b);
#endif
}

inline v128 gv_subs_s8(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_subs_epi8(a, b);
#elif defined(ARCH_ARM64)
	return vqsubq_s8(a, b);
#endif
}

inline v128 gv_subs_s16(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_subs_epi16(a, b);
#elif defined(ARCH_ARM64)
	return vqsubq_s16(a, b);
#endif
}

inline v128 gv_subs_s32(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	const v128 d = _mm_sub_epi32(a, b);
	const v128 m = (a ^ b) & (a ^ d); // overflow bit
	const v128 x = _mm_srai_epi32(m, 31);
	return _mm_or_si128(_mm_andnot_si128(x, d), _mm_and_si128(x, _mm_xor_si128(_mm_srli_epi32(x, 1), _mm_srai_epi32(a, 31))));
#elif defined(ARCH_ARM64)
	return vqsubq_s32(a, b);
#endif
}

inline v128 gv_subus_u8(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_subs_epu8(a, b);
#elif defined(ARCH_ARM64)
	return vqsubq_u8(a, b);
#endif
}

inline v128 gv_subus_u16(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_subs_epu16(a, b);
#elif defined(ARCH_ARM64)
	return vqsubq_u16(a, b);
#endif
}

inline v128 gv_subus_u32(const v128& a, const v128& b)
{
#if defined(__SSE4_1__)
	return _mm_sub_epi32(a, _mm_min_epu32(a, b));
#elif defined(ARCH_X64)
	const auto sign = _mm_set1_epi32(smin);
	return _mm_andnot_si128(_mm_cmpgt_epi32(_mm_xor_si128(b, sign), _mm_xor_si128(a, sign)), _mm_sub_epi32(a, b));
#elif defined(ARCH_ARM64)
	return vqsubq_u32(a, b);
#endif
}

inline v128 gv_subfs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_sub_ps(a, b);
#elif defined(ARCH_ARM64)
	return vsubq_f32(a, b);
#endif
}

inline v128 gv_subfd(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_sub_pd(a, b);
#elif defined(ARCH_ARM64)
	return vsubq_f64(a, b);
#endif
}

inline v128 gv_maxu8(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_max_epu8(a, b);
#elif defined(ARCH_ARM64)
	return vmaxq_u8(a, b);
#endif
}

inline v128 gv_maxu16(const v128& a, const v128& b)
{
#if defined(__SSE4_1__)
	return _mm_max_epu16(a, b);
#elif defined(ARCH_X64)
	return _mm_add_epi16(_mm_subs_epu16(a, b), b);
#elif defined(ARCH_ARM64)
	return vmaxq_u16(a, b);
#endif
}

inline v128 gv_maxu32(const v128& a, const v128& b)
{
#if defined(__SSE4_1__)
	return _mm_max_epu32(a, b);
#elif defined(ARCH_X64)
	const __m128i s = _mm_set1_epi32(smin);
	const __m128i m = _mm_cmpgt_epi32(_mm_xor_si128(a, s), _mm_xor_si128(b, s));
	return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b));
#elif defined(ARCH_ARM64)
	return vmaxq_u32(a, b);
#endif
}

inline v128 gv_maxs8(const v128& a, const v128& b)
{
#if defined(__SSE4_1__)
	return _mm_max_epi8(a, b);
#elif defined(ARCH_X64)
	const __m128i m = _mm_cmpgt_epi8(a, b);
	return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b));
#elif defined(ARCH_ARM64)
	return vmaxq_s8(a, b);
#endif
}

inline v128 gv_maxs16(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_max_epi16(a, b);
#elif defined(ARCH_ARM64)
	return vmaxq_s16(a, b);
#endif
}

inline v128 gv_maxs32(const v128& a, const v128& b)
{
#if defined(__SSE4_1__)
	return _mm_max_epi32(a, b);
#elif defined(ARCH_X64)
	const __m128i m = _mm_cmpgt_epi32(a, b);
	return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b));
#elif defined(ARCH_ARM64)
	return vmaxq_s32(a, b);
#endif
}

inline v128 gv_maxfs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_and_ps(_mm_max_ps(a, b), _mm_max_ps(b, a));
#elif defined(ARCH_ARM64)
	return vmaxq_f32(a, b);
#endif
}

inline v128 gv_minu8(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_min_epu8(a, b);
#elif defined(ARCH_ARM64)
	return vminq_u8(a, b);
#endif
}

inline v128 gv_minu16(const v128& a, const v128& b)
{
#if defined(__SSE4_1__)
	return _mm_min_epu16(a, b);
#elif defined(ARCH_X64)
	return _mm_sub_epi16(a, _mm_subs_epu16(a, b));
#elif defined(ARCH_ARM64)
	return vminq_u16(a, b);
#endif
}

inline v128 gv_minu32(const v128& a, const v128& b)
{
#if defined(__SSE4_1__)
	return _mm_min_epu32(a, b);
#elif defined(ARCH_X64)
	const __m128i s = _mm_set1_epi32(smin);
	const __m128i m = _mm_cmpgt_epi32(_mm_xor_si128(a, s), _mm_xor_si128(b, s));
	return _mm_or_si128(_mm_andnot_si128(m, a), _mm_and_si128(m, b));
#elif defined(ARCH_ARM64)
	return vminq_u32(a, b);
#endif
}

inline v128 gv_mins8(const v128& a, const v128& b)
{
#if defined(__SSE4_1__)
	return _mm_min_epi8(a, b);
#elif defined(ARCH_X64)
	const __m128i m = _mm_cmpgt_epi8(a, b);
	return _mm_or_si128(_mm_andnot_si128(m, a), _mm_and_si128(m, b));
#elif defined(ARCH_ARM64)
	return vminq_s8(a, b);
#endif
}

inline v128 gv_mins16(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_min_epi16(a, b);
#elif defined(ARCH_ARM64)
	return vminq_s16(a, b);
#endif
}

inline v128 gv_mins32(const v128& a, const v128& b)
{
#if defined(__SSE4_1__)
	return _mm_min_epi32(a, b);
#elif defined(ARCH_X64)
	const __m128i m = _mm_cmpgt_epi32(a, b);
	return _mm_or_si128(_mm_andnot_si128(m, a), _mm_and_si128(m, b));
#elif defined(ARCH_ARM64)
	return vminq_s32(a, b);
#endif
}

inline v128 gv_minfs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_or_ps(_mm_min_ps(a, b), _mm_min_ps(b, a));
#elif defined(ARCH_ARM64)
	return vminq_f32(a, b);
#endif
}

inline v128 gv_eq8(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_cmpeq_epi8(a, b);
#elif defined(ARCH_ARM64)
	return vceqq_s8(a, b);
#endif
}

inline v128 gv_eq16(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_cmpeq_epi16(a, b);
#elif defined(ARCH_ARM64)
	return vceqq_s16(a, b);
#endif
}

inline v128 gv_eq32(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_cmpeq_epi32(a, b);
#elif defined(ARCH_ARM64)
	return vceqq_s32(a, b);
#endif
}

// Ordered and equal
inline v128 gv_eqfs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_cmpeq_ps(a, b);
#elif defined(ARCH_ARM64)
	return vceqq_f32(a, b);
#endif
}

// Unordered or not equal
inline v128 gv_neqfs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_cmpneq_ps(a, b);
#elif defined(ARCH_ARM64)
	return ~vceqq_f32(a, b);
#endif
}

inline v128 gv_gtu8(const v128& a, const v128& b)
{
#if defined(__AVX512VL__) && defined(__AVX512BW__)
	return _mm_movm_epi8(_mm_cmpgt_epu8_mask(a, b));
#elif defined(ARCH_X64)
	return _mm_cmpeq_epi8(_mm_cmpeq_epi8(a, _mm_min_epu8(a, b)), _mm_setzero_si128());
#elif defined(ARCH_ARM64)
	return vcgtq_u8(a, b);
#endif
}

inline v128 gv_gtu16(const v128& a, const v128& b)
{
#if defined(__AVX512VL__) && defined(__AVX512BW__)
	return _mm_movm_epi16(_mm_cmpgt_epu16_mask(a, b));
#elif defined(__SSE4_1__)
	return _mm_cmpeq_epi16(_mm_cmpeq_epi16(a, _mm_min_epu16(a, b)), _mm_setzero_si128());
#elif defined(ARCH_X64)
	return _mm_cmpeq_epi16(_mm_cmpeq_epi16(_mm_subs_epu16(a, b), _mm_setzero_si128()), _mm_setzero_si128());
#elif defined(ARCH_ARM64)
	return vcgtq_u16(a, b);
#endif
}

inline v128 gv_gtu32(const v128& a, const v128& b)
{
#if defined(__AVX512VL__) && defined(__AVX512DQ__)
	return _mm_movm_epi32(_mm_cmpgt_epu32_mask(a, b));
#elif defined(__SSE4_1__)
	return _mm_cmpeq_epi32(_mm_cmpeq_epi32(a, _mm_min_epu32(a, b)), _mm_setzero_si128());
#elif defined(ARCH_X64)
	const auto sign = _mm_set1_epi32(smin);
	return _mm_cmpgt_epi32(_mm_xor_si128(a, sign), _mm_xor_si128(b, sign));
#elif defined(ARCH_ARM64)
	return vcgtq_u32(a, b);
#endif
}

// Ordered and greater than
inline v128 gv_gtfs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_cmpgt_ps(a, b);
#elif defined(ARCH_ARM64)
	return vcgtq_f32(a, b);
#endif
}

// Ordered and less than
inline v128 gv_ltfs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_cmplt_ps(a, b);
#elif defined(ARCH_ARM64)
	return vcltq_f32(a, b);
#endif
}

// Unordered or less or equal
inline v128 gv_ngtfs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_cmpngt_ps(a, b);
#elif defined(ARCH_ARM64)
	return ~vcgtq_f32(a, b);
#endif
}

// Unordered or greater or equal
inline v128 gv_nlefs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_cmpnle_ps(a, b);
#elif defined(ARCH_ARM64)
	return ~vcleq_f32(a, b);
#endif
}

inline v128 gv_geu8(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_cmpeq_epi8(b, _mm_min_epu8(a, b));
#elif defined(ARCH_ARM64)
	return vcgeq_u8(a, b);
#endif
}

inline v128 gv_geu16(const v128& a, const v128& b)
{
#if defined(__SSE4_1__)
	return _mm_cmpeq_epi16(b, _mm_min_epu16(a, b));
#elif defined(ARCH_X64)
	return _mm_cmpeq_epi16(_mm_subs_epu16(b, a), _mm_setzero_si128());
#elif defined(ARCH_ARM64)
	return vcgeq_u16(a, b);
#endif
}

inline v128 gv_geu32(const v128& a, const v128& b)
{
#if defined(__SSE4_1__)
	return _mm_cmpeq_epi32(b, _mm_min_epu32(a, b));
#elif defined(ARCH_X64)
	const auto sign = _mm_set1_epi32(smin);
	return _mm_cmpeq_epi32(_mm_cmpgt_epi32(_mm_xor_si128(b, sign), _mm_xor_si128(a, sign)), _mm_setzero_si128());
#elif defined(ARCH_ARM64)
	return vcgeq_u32(a, b);
#endif
}

// Ordered and not less than
inline v128 gv_gefs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_cmpge_ps(a, b);
#elif defined(ARCH_ARM64)
	return vcgeq_f32(a, b);
#endif
}

// Unordered or less than
inline v128 gv_ngefs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_cmpnge_ps(a, b);
#elif defined(ARCH_ARM64)
	return ~vcgeq_f32(a, b);
#endif
}

inline v128 gv_gts8(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_cmpgt_epi8(a, b);
#elif defined(ARCH_ARM64)
	return vcgtq_s8(a, b);
#endif
}

inline v128 gv_gts16(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_cmpgt_epi16(a, b);
#elif defined(ARCH_ARM64)
	return vcgtq_s16(a, b);
#endif
}

inline v128 gv_gts32(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_cmpgt_epi32(a, b);
#elif defined(ARCH_ARM64)
	return vcgtq_s32(a, b);
#endif
}

inline v128 gv_avgu8(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_avg_epu8(a, b);
#elif defined(ARCH_ARM64)
	return vrhaddq_u8(a, b);
#endif
}

inline v128 gv_avgu16(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_avg_epu16(a, b);
#elif defined(ARCH_ARM64)
	return vrhaddq_u16(a, b);
#endif
}

inline v128 gv_avgu32(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	const auto ones = _mm_set1_epi32(-1);
	const auto summ = gv_sub32(gv_add32(a, b), ones);
	const auto carry = _mm_slli_epi32(gv_geu32(a, summ), 31);
	return _mm_or_si128(carry, _mm_srli_epi32(summ, 1));
#elif defined(ARCH_ARM64)
	return vrhaddq_u32(a, b);
#endif
}

inline v128 gv_avgs8(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	const v128 sign = _mm_set1_epi8(smin);
	return gv_avgu8(a ^ sign, b ^ sign) ^ sign;
#elif defined(ARCH_ARM64)
	return vrhaddq_s8(a, b);
#endif
}

inline v128 gv_avgs16(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	const v128 sign = _mm_set1_epi16(smin);
	return gv_avgu16(a ^ sign, b ^ sign) ^ sign;
#elif defined(ARCH_ARM64)
	return vrhaddq_s16(a, b);
#endif
}

inline v128 gv_avgs32(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	const v128 sign = _mm_set1_epi32(smin);
	return gv_avgu32(a ^ sign, b ^ sign) ^ sign;
#elif defined(ARCH_ARM64)
	return vrhaddq_s32(a, b);
#endif
}

inline v128 gv_fmafs(const v128& a, const v128& b, const v128& c)
{
#if defined(ARCH_X64) && defined(__FMA__)
	return _mm_fmadd_ps(a, b, c);
#elif defined(__FMA4__)
	return _mm_macc_ps(a, b, c);
#elif defined(ARCH_X64)
	// This is inaccurate implementation
#ifdef __AVX__
	const __m128 r = _mm256_cvtpd_ps(_mm256_add_ps(_mm256_mul_ps(_mm256_cvtps_pd(a), _mm256_cvtps_pd(b)), _mm256_cvtps_pd(c)));
#else
	const __m128d a0 = _mm_cvtps_pd(a);
	const __m128d a1 = _mm_cvtps_pd(_mm_movehl_ps(a, a));
	const __m128d b0 = _mm_cvtps_pd(b);
	const __m128d b1 = _mm_cvtps_pd(_mm_movehl_ps(b, b));
	const __m128d c0 = _mm_cvtps_pd(c);
	const __m128d c1 = _mm_cvtps_pd(_mm_movehl_ps(c, c));
	const __m128d m0 = _mm_mul_pd(a0, b0);
	const __m128d m1 = _mm_mul_pd(a1, b1);
	const __m128d r0 = _mm_add_pd(m0, c0);
	const __m128d r1 = _mm_add_pd(m1, c1);
	const __m128 r = _mm_movelh_ps(_mm_cvtpd_ps(r0), _mm_cvtpd_ps(r1));
#endif
	return r;
#elif defined(ARCH_ARM64)
	return vfmaq_f32(c, a, b);
#else
	v128 r;
	for (int i = 0; i < 4; i++)
	{
		r._f[i] = std::fmaf(a._f[i], b._f[i], c._f[i]);
	}
	return r;
#endif
}

inline v128 gv_muladdfs(const v128& a, const v128& b, const v128& c)
{
#if defined(ARCH_X64) && defined(__FMA__)
	return _mm_fmadd_ps(a, b, c);
#elif defined(__FMA4__)
	return _mm_macc_ps(a, b, c);
#elif defined(ARCH_ARM64)
	return vfmaq_f32(c, a, b);
#elif defined(ARCH_X64)
	return _mm_add_ps(_mm_mul_ps(a, b), c);
#endif
}

// -> ssat((a * b * 2 + (c << 16) + 0x8000) >> 16)
inline v128 gv_rmuladds_hds16(const v128& a, const v128& b, const v128& c)
{
#if defined(ARCH_ARM64)
	return vqrdmlahq_s16(c, a, b);
#elif defined(ARCH_X64)
	const auto x80 = _mm_set1_epi16(0x80); // 0x80 * 0x80 = 0x4000, add this to the product
	const auto al = _mm_unpacklo_epi16(a, x80);
	const auto ah = _mm_unpackhi_epi16(a, x80);
	const auto bl = _mm_unpacklo_epi16(b, x80);
	const auto bh = _mm_unpackhi_epi16(b, x80);
	const auto ml = _mm_srai_epi32(_mm_madd_epi16(al, bl), 15);
	const auto mh = _mm_srai_epi32(_mm_madd_epi16(ah, bh), 15);
	const auto cl = _mm_srai_epi32(_mm_unpacklo_epi16(_mm_setzero_si128(), c), 16);
	const auto ch = _mm_srai_epi32(_mm_unpackhi_epi16(_mm_setzero_si128(), c), 16);
	const auto sl = _mm_add_epi32(ml, cl);
	const auto sh = _mm_add_epi32(mh, ch);
	return _mm_packs_epi32(sl, sh);
#endif
}

// -> ssat((a * b * 2 + 0x8000) >> 16)
inline v128 gv_rmuls_hds16(const v128& a, const v128& b)
{
#if defined(ARCH_ARM64)
	return vqrdmulhq_s16(a, b);
#elif defined(ARCH_X64)
	const auto x80 = _mm_set1_epi16(0x80); // 0x80 * 0x80 = 0x4000, add this to the product
	const auto al = _mm_unpacklo_epi16(a, x80);
	const auto ah = _mm_unpackhi_epi16(a, x80);
	const auto bl = _mm_unpacklo_epi16(b, x80);
	const auto bh = _mm_unpackhi_epi16(b, x80);
	const auto ml = _mm_srai_epi32(_mm_madd_epi16(al, bl), 15);
	const auto mh = _mm_srai_epi32(_mm_madd_epi16(ah, bh), 15);
	return _mm_packs_epi32(ml, mh);
#endif
}

// -> ssat((a * b * 2) >> 16)
inline v128 gv_muls_hds16(const v128& a, const v128& b)
{
#if defined(ARCH_ARM64)
	return vqdmulhq_s16(a, b);
#elif defined(ARCH_X64)
	const auto m = _mm_or_si128(_mm_srli_epi16(_mm_mullo_epi16(a, b), 15), _mm_slli_epi16(_mm_mulhi_epi16(a, b), 1));
	const auto s = _mm_cmpeq_epi16(m, _mm_set1_epi16(-0x8000)); // detect special case (positive 0x8000)
	return _mm_xor_si128(m, s);
#endif
}

inline v128 gv_muladd16(const v128& a, const v128& b, const v128& c)
{
#if defined(ARCH_X64)
	return _mm_add_epi16(_mm_mullo_epi16(a, b), c);
#elif defined(ARCH_ARM64)
	return vmlaq_s16(c, a, b);
#endif
}

inline v128 gv_mul16(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_mullo_epi16(a, b);
#elif defined(ARCH_ARM64)
	return vmulq_s16(a, b);
#endif
}

inline v128 gv_mul32(const v128& a, const v128& b)
{
#if defined(__SSE4_1__)
	return _mm_mullo_epi32(a, b);
#elif defined(ARCH_X64)
	const __m128i lows = _mm_shuffle_epi32(_mm_mul_epu32(a, b), 8);
	const __m128i highs = _mm_shuffle_epi32(_mm_mul_epu32(_mm_srli_epi64(a, 32), _mm_srli_epi64(b, 32)), 8);
	return _mm_unpacklo_epi32(lows, highs);
#elif defined(ARCH_ARM64)
	return vmulq_s32(a, b);
#endif
}

inline v128 gv_mulfs(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_mul_ps(a, b);
#elif defined(ARCH_ARM64)
	return vmulq_f32(a, b);
#endif
}

inline v128 gv_hadds8x2(const v128& a)
{
#if defined(__SSSE3__)
	return _mm_maddubs_epi16(_mm_set1_epi8(1), a);
#elif defined(ARCH_X64)
	return _mm_add_epi16(_mm_srai_epi16(a, 8), _mm_srai_epi16(_mm_slli_epi16(a, 8), 8));
#elif defined(ARCH_ARM64)
	return vpaddlq_s8(a);
#endif
}

inline v128 gv_hadds8x4(const v128& a)
{
#if (defined(__AVX512VL__) && defined(__AVX512VNNI__)) || defined(__AVXVNNI__)
	return _mm_dpbusd_epi32(_mm_setzero_si128(), _mm_set1_epi8(1), a);
#elif defined(__SSSE3__)
	return _mm_madd_epi16(_mm_maddubs_epi16(_mm_set1_epi8(1), a), _mm_set1_epi16(1));
#elif defined(ARCH_X64)
	return _mm_madd_epi16(_mm_add_epi16(_mm_srai_epi16(a, 8), _mm_srai_epi16(_mm_slli_epi16(a, 8), 8)), _mm_set1_epi16(1));
#elif defined(ARCH_ARM64)
	return vpaddlq_s16(vpaddlq_s8(a));
#endif
}

inline v128 gv_haddu8x2(const v128& a)
{
#if defined(__SSSE3__)
	return _mm_maddubs_epi16(a, _mm_set1_epi8(1));
#elif defined(ARCH_X64)
	return _mm_add_epi16(_mm_srli_epi16(a, 8), _mm_and_si128(a, _mm_set1_epi16(0x00ff)));
#elif defined(ARCH_ARM64)
	return vpaddlq_u8(a);
#endif
}

inline v128 gv_haddu8x4(const v128& a)
{
#if (defined(__AVX512VL__) && defined(__AVX512VNNI__)) || defined(__AVXVNNI__)
	return _mm_dpbusd_epi32(_mm_setzero_si128(), a, _mm_set1_epi8(1));
#elif defined(__SSSE3__)
	return _mm_madd_epi16(_mm_maddubs_epi16(a, _mm_set1_epi8(1)), _mm_set1_epi16(1));
#elif defined(ARCH_X64)
	return _mm_madd_epi16(_mm_add_epi16(_mm_srli_epi16(a, 8), _mm_and_si128(a, _mm_set1_epi16(0x00ff))), _mm_set1_epi16(1));
#elif defined(ARCH_ARM64)
	return vpaddlq_u16(vpaddlq_u8(a));
#endif
}

inline v128 gv_hadds16x2(const v128& a)
{
#if defined(ARCH_X64)
	return _mm_madd_epi16(a, _mm_set1_epi16(1));
#elif defined(ARCH_ARM64)
	return vpaddlq_s16(a);
#endif
}

// Unsigned bytes from a, signed bytes from b, 32-bit accumulator c
inline v128 gv_dotu8s8x4(const v128& a, const v128& b, const v128& c)
{
#if (defined(__AVX512VL__) && defined(__AVX512VNNI__)) || defined(__AVXVNNI__)
	return _mm_dpbusd_epi32(c, a, b);
#elif defined(ARCH_X64)
	const __m128i ah = _mm_srli_epi16(a, 8);
	const __m128i al = _mm_and_si128(a, _mm_set1_epi16(0x00ff));
	const __m128i bh = _mm_srai_epi16(b, 8);
	const __m128i bl = _mm_srai_epi16(_mm_slli_epi16(b, 8), 8);
	const __m128i mh = _mm_madd_epi16(ah, bh);
	const __m128i ml = _mm_madd_epi16(al, bl);
	const __m128i x = _mm_add_epi32(mh, ml);
	return _mm_add_epi32(c, x);
#elif defined(__ARM_FEATURE_MATMUL_INT8)
	return vusdotq_s32(c, a, b);
#elif defined(ARCH_ARM64)
    const auto l = vpaddlq_s16(vmulq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(a))), vmovl_s8(vget_low_s8(b))));
	const auto h = vpaddlq_s16(vmulq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(a))), vmovl_s8(vget_high_s8(b))));
    return vaddq_s32(c, vaddq_s32(vuzp1q_s32(l, h), vuzp2q_s32(l, h)));
#endif
}

inline v128 gv_dotu8x4(const v128& a, const v128& b, const v128& c)
{
#if defined(ARCH_X64)
	const __m128i ah = _mm_srli_epi16(a, 8);
	const __m128i al = _mm_and_si128(a, _mm_set1_epi16(0x00ff));
	const __m128i bh = _mm_srli_epi16(b, 8);
	const __m128i bl = _mm_and_si128(b, _mm_set1_epi16(0x00ff));
	const __m128i mh = _mm_madd_epi16(ah, bh);
	const __m128i ml = _mm_madd_epi16(al, bl);
	const __m128i x = _mm_add_epi32(mh, ml);
	return _mm_add_epi32(c, x);
#elif defined(__ARM_FEATURE_DOTPROD)
	return vdotq_u32(c, a, b);
#elif defined(ARCH_ARM64)
    const auto l = vpaddlq_u16(vmulq_u16(vmovl_u8(vget_low_u8(a)), vmovl_u8(vget_low_u8(b))));
	const auto h = vpaddlq_u16(vmulq_u16(vmovl_u8(vget_high_u8(a)), vmovl_u8(vget_high_u8(b))));
    return vaddq_u32(c, vaddq_u32(vuzp1q_u32(l, h), vuzp2q_u32(l, h)));
#endif
}

inline v128 gv_dots16x2(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_madd_epi16(a, b);
#elif defined(ARCH_ARM64)
	const auto ml = vmull_s16(vget_low_s16(a), vget_low_s16(b));
	const auto mh = vmull_s16(vget_high_s16(a), vget_high_s16(b));
	const auto sl = vpadd_s32(vget_low_s32(ml), vget_high_s32(ml));
	const auto sh = vpadd_s32(vget_low_s32(mh), vget_high_s32(mh));
	return vcombine_s32(sl, sh);
#endif
}

// Signed s16 from a and b, 32-bit accumulator c
inline v128 gv_dots16x2(const v128& a, const v128& b, const v128& c)
{
#if (defined(__AVX512VL__) && defined(__AVX512VNNI__)) || defined(__AVXVNNI__)
	return _mm_dpwssd_epi32(c, a, b);
#else
	return gv_add32(c, gv_dots16x2(a, b));
#endif
}

inline v128 gv_dotu16x2(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	const auto ml = _mm_mullo_epi16(a, b); // low results
	const auto mh = _mm_mulhi_epu16(a, b); // high results
	const auto ls = _mm_add_epi32(_mm_srli_epi32(ml, 16), _mm_and_si128(ml, _mm_set1_epi32(0x0000ffff)));
	const auto hs = _mm_add_epi32(_mm_slli_epi32(mh, 16), _mm_and_si128(mh, _mm_set1_epi32(0xffff0000)));
	return _mm_add_epi32(ls, hs);
#elif defined(ARCH_ARM64)
	const auto ml = vmull_u16(vget_low_u16(a), vget_low_u16(b));
	const auto mh = vmull_u16(vget_high_u16(a), vget_high_u16(b));
	const auto sl = vpadd_u32(vget_low_u32(ml), vget_high_u32(ml));
	const auto sh = vpadd_u32(vget_low_u32(mh), vget_high_u32(mh));
	return vcombine_u32(sl, sh);
#endif
}

// Signed s16 from a and b, 32-bit accumulator c; signed saturation
inline v128 gv_dots_s16x2(const v128& a, const v128& b, const v128& c)
{
#if (defined(__AVX512VL__) && defined(__AVX512VNNI__)) || defined(__AVXVNNI__)
	return _mm_dpwssds_epi32(c, a, b);
#else
	const auto ab = gv_dots16x2(a, b);
	const auto s0 = gv_adds_s32(ab, c);
	const auto s1 = gv_eq32(ab, gv_bcst32(0x80000000)); // +0x80000000, negative c -> c^0x80000000; otherwise 0x7fffffff
	const auto s2 = gv_select32(gv_gts32(gv_bcst32(0), c), gv_xor32(c, gv_bcst32(0x80000000)), gv_bcst32(0x7fffffff));
	return gv_select32(s1, s2, s0);
#endif
}

// Multiply s16 elements 0, 2, 4, 6 to produce s32 results in corresponding lanes
inline v128 gv_mul_even_s16(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	const auto c = _mm_set1_epi32(0x0000ffff);
	return _mm_madd_epi16(_mm_and_si128(a, c), _mm_and_si128(b, c));
#else
	// TODO
	return gv_mul32(gv_sar32(gv_shl32(a, 16), 16), gv_sar32(gv_shl32(b, 16), 16));
#endif
}

// Multiply u16 elements 0, 2, 4, 6 to produce u32 results in corresponding lanes
inline v128 gv_mul_even_u16(const v128& a, const v128& b)
{
#if defined(__SSE4_1__) || defined(ARCH_ARM64)
	const auto c = gv_bcst32(0x0000ffff);
	return gv_mul32(a & c, b & c);
#elif defined(ARCH_X64)
	const auto ml = _mm_mullo_epi16(a, b);
	const auto mh = _mm_mulhi_epu16(a, b);
	return _mm_or_si128(_mm_and_si128(ml, _mm_set1_epi32(0x0000ffff)), _mm_slli_epi32(mh, 16));
#endif
}

// Multiply s16 elements 1, 3, 5, 7 to produce s32 results in corresponding lanes
inline v128 gv_mul_odds_s16(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_madd_epi16(_mm_srli_epi32(a, 16), _mm_srli_epi32(b, 16));
#else
	return gv_mul32(gv_sar32(a, 16), gv_sar32(b, 16));
#endif
}

// Multiply u16 elements 1, 3, 5, 7 to produce u32 results in corresponding lanes
inline v128 gv_mul_odds_u16(const v128& a, const v128& b)
{
#if defined(__SSE4_1__) || defined(ARCH_ARM64)
	return gv_mul32(gv_shr32(a, 16), gv_shr32(b, 16));
#elif defined(ARCH_X64)
	const auto ml = _mm_mullo_epi16(a, b);
	const auto mh = _mm_mulhi_epu16(a, b);
	return _mm_or_si128(_mm_and_si128(mh, _mm_set1_epi32(0xffff0000)), _mm_srli_epi32(ml, 16));
#endif
}

inline v128 gv_cvts32_tofs(const v128& src)
{
#if defined(ARCH_X64)
	return _mm_cvtepi32_ps(src);
#elif defined(ARCH_ARM64)
	return vcvtq_f32_s32(src);
#endif
}

inline v128 gv_cvtu32_tofs(const v128& src)
{
#if defined(__AVX512VL__)
	return _mm_cvtepu32_ps(src);
#elif defined(ARCH_X64)
	const auto fix = _mm_and_ps(_mm_castsi128_ps(_mm_srai_epi32(src, 31)), _mm_set1_ps(0x80000000));
	return _mm_add_ps(_mm_cvtepi32_ps(_mm_and_si128(src, _mm_set1_epi32(0x7fffffff))), fix);
#elif defined(ARCH_ARM64)
	return vcvtq_f32_u32(src);
#endif
}

inline v128 gv_cvtfs_tos32(const v128& src)
{
#if defined(ARCH_X64)
	return _mm_cvttps_epi32(src);
#elif defined(ARCH_ARM64)
	return vcvtq_s32_f32(src);
#endif
}

inline v128 gv_cvtfs_tou32(const v128& src)
{
#if defined(__AVX512VL__)
	return _mm_cvttps_epu32(src);
#elif defined(ARCH_X64)
	const auto c1 = _mm_cvttps_epi32(src);
	const auto s1 = _mm_srai_epi32(c1, 31);
	const auto c2 = _mm_cvttps_epi32(_mm_sub_ps(src, _mm_set1_ps(2147483648.)));
	return _mm_or_si128(c1, _mm_and_si128(c2, s1));
#elif defined(ARCH_ARM64)
	return vcvtq_u32_f32(src);
#endif
}

inline bool gv_testz(const v128& a)
{
#if defined(__SSE4_1__)
	return !!_mm_testz_si128(a, a);
#elif defined(ARCH_X64)
	return _mm_cvtsi128_si64(_mm_packs_epi32(a, a)) == 0;
#elif defined(ARCH_ARM64)
	return std::bit_cast<s64>(vqmovn_s32(a)) == 0;
#else
	return !(a._u64[0] | a._u64[1]);
#endif
}

// Same as gv_testz but tuned for pairing with gv_testall1
inline bool gv_testall0(const v128& a)
{
#if defined(__SSE4_1__)
	return !!_mm_testz_si128(a, _mm_set1_epi32(-1));
#elif defined(ARCH_X64)
	return _mm_cvtsi128_si64(_mm_packs_epi32(a, a)) == 0;
#elif defined(ARCH_ARM64)
	return std::bit_cast<s64>(vqmovn_s32(a)) == 0;
#else
	return !(a._u64[0] | a._u64[1]);
#endif
}

inline bool gv_testall1(const v128& a)
{
#if defined(__SSE4_1__)
	return !!_mm_test_all_ones(a);
#elif defined(ARCH_X64)
	return _mm_cvtsi128_si64(_mm_packs_epi32(a, a)) == -1;
#elif defined(ARCH_ARM64)
	return std::bit_cast<s64>(vqmovn_s32(a)) == -1;
#else
	return (a._u64[0] & a._u64[1]) == UINT64_MAX;
#endif
}

// result = (~a) & (b)
inline v128 gv_andn(const v128& a, const v128& b)
{
#if defined(ARCH_X64)
	return _mm_andnot_si128(a, b);
#elif defined(ARCH_ARM64)
	return vbicq_s32(b, a);
#endif
}

// Select elements; _cmp must be result of SIMD comparison; undefined otherwise
inline v128 gv_select8(const v128& _cmp, const v128& _true, const v128& _false)
{
#if defined(__SSE4_1__)
	return _mm_blendv_epi8(_false, _true, _cmp);
#elif defined(ARCH_ARM64)
	return vbslq_u8(_cmp, _true, _false);
#else
	return (_cmp & _true) | gv_andn(_cmp, _false);
#endif
}

// Select elements; _cmp must be result of SIMD comparison; undefined otherwise
inline v128 gv_select16(const v128& _cmp, const v128& _true, const v128& _false)
{
#if defined(__SSE4_1__)
	return _mm_blendv_epi8(_false, _true, _cmp);
#elif defined(ARCH_ARM64)
	return vbslq_u16(_cmp, _true, _false);
#else
	return (_cmp & _true) | gv_andn(_cmp, _false);
#endif
}

// Select elements; _cmp must be result of SIMD comparison; undefined otherwise
inline v128 gv_select32(const v128& _cmp, const v128& _true, const v128& _false)
{
#if defined(__SSE4_1__)
	return _mm_blendv_epi8(_false, _true, _cmp);
#elif defined(ARCH_ARM64)
	return vbslq_u32(_cmp, _true, _false);
#else
	return (_cmp & _true) | gv_andn(_cmp, _false);
#endif
}

// Select elements; _cmp must be result of SIMD comparison; undefined otherwise
inline v128 gv_selectfs(const v128& _cmp, const v128& _true, const v128& _false)
{
#if defined(__SSE4_1__)
	return _mm_blendv_ps(_false, _true, _cmp);
#elif defined(ARCH_ARM64)
	return vbslq_f32(_cmp, _true, _false);
#else
	return _mm_or_ps(_mm_and_ps(_cmp, _true), _mm_andnot_ps(_cmp, _false));
#endif
}

inline v128 gv_unpacklo8(const v128& lows, const v128& highs)
{
#if defined(ARCH_X64)
	return _mm_unpacklo_epi8(lows, highs);
#elif defined(ARCH_ARM64)
	return vzip1q_s8(lows, highs);
#endif
}

inline v128 gv_extend_lo_s8(const v128& vec)
{
#if defined(__SSE4_1__)
	return _mm_cvtepi8_epi16(vec);
#elif defined(ARCH_X64)
	return _mm_srai_epi16(_mm_unpacklo_epi8(_mm_undefined_si128(), vec), 8);
#elif defined(ARCH_ARM64)
	return int16x8_t(vmovl_s8(vget_low_s8(vec)));
#endif
}

inline v128 gv_extend_hi_s8(const v128& vec)
{
#if defined(__SSE4_1__)
	return _mm_cvtepi8_epi16(_mm_loadu_si64(vec._bytes + 8));
#elif defined(ARCH_X64)
	return _mm_srai_epi16(_mm_unpackhi_epi8(_mm_undefined_si128(), vec), 8);
#elif defined(ARCH_ARM64)
	return int16x8_t(vmovl_s8(vget_high_s8(vec)));
#endif
}

inline v128 gv_unpacklo16(const v128& lows, const v128& highs)
{
#if defined(ARCH_X64)
	return _mm_unpacklo_epi16(lows, highs);
#elif defined(ARCH_ARM64)
	return vzip1q_s16(lows, highs);
#endif
}

inline v128 gv_extend_lo_s16(const v128& vec)
{
#if defined(__SSE4_1__)
	return _mm_cvtepi16_epi32(vec);
#elif defined(ARCH_X64)
	return _mm_srai_epi32(_mm_unpacklo_epi16(_mm_undefined_si128(), vec), 16);
#elif defined(ARCH_ARM64)
	return int32x4_t(vmovl_s16(vget_low_s16(vec)));
#endif
}

inline v128 gv_extend_hi_s16(const v128& vec)
{
#if defined(__SSE4_1__)
	return _mm_cvtepi16_epi32(_mm_loadu_si64(vec._bytes + 8));
#elif defined(ARCH_X64)
	return _mm_srai_epi32(_mm_unpackhi_epi16(_mm_undefined_si128(), vec), 16);
#elif defined(ARCH_ARM64)
	return int32x4_t(vmovl_s16(vget_high_s16(vec)));
#endif
}

inline v128 gv_unpacklo32(const v128& lows, const v128& highs)
{
#if defined(ARCH_X64)
	return _mm_unpacklo_epi32(lows, highs);
#elif defined(ARCH_ARM64)
	return vzip1q_s32(lows, highs);
#endif
}

inline v128 gv_unpackhi8(const v128& lows, const v128& highs)
{
#if defined(ARCH_X64)
	return _mm_unpackhi_epi8(lows, highs);
#elif defined(ARCH_ARM64)
	return vzip2q_s8(lows, highs);
#endif
}

inline v128 gv_unpackhi16(const v128& lows, const v128& highs)
{
#if defined(ARCH_X64)
	return _mm_unpackhi_epi16(lows, highs);
#elif defined(ARCH_ARM64)
	return vzip2q_s16(lows, highs);
#endif
}

inline v128 gv_unpackhi32(const v128& lows, const v128& highs)
{
#if defined(ARCH_X64)
	return _mm_unpackhi_epi32(lows, highs);
#elif defined(ARCH_ARM64)
	return vzip2q_s32(lows, highs);
#endif
}

inline bool v128::operator==(const v128& b) const
{
#if defined(ARCH_X64)
	return gv_testz(_mm_xor_si128(*this, b));
#else
	return gv_testz(*this ^ b);
#endif
}

inline v128 v128::operator|(const v128& rhs) const
{
#if defined(ARCH_X64)
	return _mm_or_si128(*this, rhs);
#elif defined(ARCH_ARM64)
	return vorrq_s32(*this, rhs);
#endif
}

inline v128 v128::operator&(const v128& rhs) const
{
#if defined(ARCH_X64)
	return _mm_and_si128(*this, rhs);
#elif defined(ARCH_ARM64)
	return vandq_s32(*this, rhs);
#endif
}

inline v128 v128::operator^(const v128& rhs) const
{
#if defined(ARCH_X64)
	return _mm_xor_si128(*this, rhs);
#elif defined(ARCH_ARM64)
	return veorq_s32(*this, rhs);
#endif
}

inline v128 v128::operator~() const
{
#if defined(ARCH_X64)
	return _mm_xor_si128(*this, _mm_set1_epi32(-1));
#elif defined(ARCH_ARM64)
	return vmvnq_u32(*this);
#endif
}

inline v128 gv_exp2_approxfs(const v128& a)
{
	// TODO
#if 0
	const auto x0 = _mm_max_ps(_mm_min_ps(a, _mm_set1_ps(127.4999961f)), _mm_set1_ps(-127.4999961f));
	const auto x1 = _mm_add_ps(x0, _mm_set1_ps(0.5f));
	const auto x2 = _mm_sub_epi32(_mm_cvtps_epi32(x1), _mm_and_si128(_mm_castps_si128(_mm_cmpnlt_ps(_mm_setzero_ps(), x1)), _mm_set1_epi32(1)));
	const auto x3 = _mm_sub_ps(x0, _mm_cvtepi32_ps(x2));
	const auto x4 = _mm_mul_ps(x3, x3);
	const auto x5 = _mm_mul_ps(x3, _mm_add_ps(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(x4, _mm_set1_ps(0.023093347705f)), _mm_set1_ps(20.20206567f)), x4), _mm_set1_ps(1513.906801f)));
	const auto x6 = _mm_mul_ps(x5, _mm_rcp_ps(_mm_sub_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(233.1842117f), x4), _mm_set1_ps(4368.211667f)), x5)));
	return _mm_mul_ps(_mm_add_ps(_mm_add_ps(x6, x6), _mm_set1_ps(1.0f)), _mm_castsi128_ps(_mm_slli_epi32(_mm_add_epi32(x2, _mm_set1_epi32(127)), 23)));
#else
	v128 r;
	for (u32 i = 0; i < 4; i++)
		r._f[i] = std::exp2f(a._f[i]);
	return r;
#endif
}

inline v128 gv_log2_approxfs(const v128& a)
{
	// TODO
#if 0
	const auto _1 = _mm_set1_ps(1.0f);
	const auto _c = _mm_set1_ps(1.442695040f);
	const auto x0 = _mm_max_ps(a, _mm_castsi128_ps(_mm_set1_epi32(0x00800000)));
	const auto x1 = _mm_or_ps(_mm_and_ps(x0, _mm_castsi128_ps(_mm_set1_epi32(0x807fffff))), _1);
	const auto x2 = _mm_rcp_ps(_mm_add_ps(x1, _1));
	const auto x3 = _mm_mul_ps(_mm_sub_ps(x1, _1), x2);
	const auto x4 = _mm_add_ps(x3, x3);
	const auto x5 = _mm_mul_ps(x4, x4);
	const auto x6 = _mm_add_ps(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(-0.7895802789f), x5), _mm_set1_ps(16.38666457f)), x5), _mm_set1_ps(-64.1409953f));
	const auto x7 = _mm_rcp_ps(_mm_add_ps(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(-35.67227983f), x5), _mm_set1_ps(312.0937664f)), x5), _mm_set1_ps(-769.6919436f)));
	const auto x8 = _mm_cvtepi32_ps(_mm_sub_epi32(_mm_srli_epi32(_mm_castps_si128(x0), 23), _mm_set1_epi32(127)));
	return _mm_add_ps(_mm_mul_ps(_mm_mul_ps(_mm_mul_ps(_mm_mul_ps(x5, x6), x7), x4), _c), _mm_add_ps(_mm_mul_ps(x4, _c), x8));
#else
	v128 r;
	for (u32 i = 0; i < 4; i++)
		r._f[i] = std::log2f(a._f[i]);
	return r;
#endif
}
