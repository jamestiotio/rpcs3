#pragma once

#include "overlays.h"
#include "overlay_progress_bar.hpp"
#include "Emu/Cell/Modules/cellMsgDialog.h"

namespace rsx
{
	namespace overlays
	{
		class message_dialog : public user_interface
		{
			label text_display;
			image_button btn_ok;
			image_button btn_cancel;

			overlay_element bottom_bar, background;
			image_view background_poster;
			std::array<progress_bar, 2> progress_bars{};
			u8 num_progress_bars = 0;
			s32 taskbar_index = 0;
			s32 taskbar_limit = 0;

			bool interactive = false;
			bool ok_only = false;
			bool cancel_only = false;

			bool custom_background_allowed = false;
			u32 background_blur_strength = 0;
			u32 background_darkening_strength = 0;
			std::unique_ptr<image_info> background_image;

			animation_color_interpolate fade_animation;

			struct text_guard_t
			{
				std::mutex mutex;
				std::string text;
				bool dirty{false};

				void set_text(std::string t)
				{
					std::lock_guard lock(mutex);
					text = std::move(t);
					dirty = true;
				}

				std::pair<bool, std::string> get_text()
				{
					if (dirty)
					{
						std::lock_guard lock(mutex);
						dirty = false;
						return { true, std::move(text) };
					}

					return { false, {} };
				}
			};

			text_guard_t text_guard{};
			std::array<text_guard_t, 2> bar_text_guard{};

		public:
			message_dialog(bool allow_custom_background = false);

			compiled_resource get_compiled() override;

			void update() override;
			void on_button_pressed(pad_button button_press, bool is_auto_repeat) override;
			void close(bool use_callback, bool stop_pad_interception) override;

			error_code show(bool is_blocking, const std::string& text, const MsgDialogType& type, std::function<void(s32 status)> on_close);

			void set_text(const std::string& text);
			void update_custom_background();

			u32 progress_bar_count() const;
			void progress_bar_set_taskbar_index(s32 index);
			error_code progress_bar_set_message(u32 index, const std::string& msg);
			error_code progress_bar_increment(u32 index, f32 value);
			error_code progress_bar_set_value(u32 index, f32 value);
			error_code progress_bar_reset(u32 index);
			error_code progress_bar_set_limit(u32 index, u32 limit);
		};
	}
}
