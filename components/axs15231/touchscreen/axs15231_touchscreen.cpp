#include "axs15231_touchscreen.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace axs15231 {

namespace {
  const char *const TAG = "axs15231.touchscreen";

  constexpr uint8_t AXS_TOUCH_POINT_LEN     = 0x06;
  constexpr uint8_t AXS_TOUCH_BUF_HEAD_LEN  = 0x02;

  constexpr uint8_t AXS_TOUCH_EVENT_TOUCH   = 0x08;

  constexpr const uint8_t AXS_READ_TOUCHPAD[11] = { 0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00 };
} // anonymous namespace

void AXS15231Touchscreen::setup() {
  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->setup();
    this->reset_pin_->digital_write(false);
    delay(5);
    this->reset_pin_->digital_write(true);
    delay(10);
  }

  if (this->interrupt_pin_ != nullptr) {
    this->interrupt_pin_->pin_mode(gpio::FLAG_INPUT);
    this->interrupt_pin_->setup();
    this->attach_interrupt_(this->interrupt_pin_, gpio::INTERRUPT_FALLING_EDGE);
  }

  // Default the raw range to the panel size, unless `calibration:` set it.
  if (this->x_raw_max_ == 0) {
    this->x_raw_max_ = this->display_->get_native_width();
  }
  if (this->y_raw_max_ == 0) {
    this->y_raw_max_ = this->display_->get_native_height();
  }
}

// No read throttle here. Touchscreen::loop() marks every current touch as
// releasing before calling this, so returning without a touch (and without
// setting skip_update_) reports a release. A throttle that returned early
// turned one held tap into press/release/press.
void AXS15231Touchscreen::update_touches() {
  uint8_t data[AXS_TOUCH_BUF_HEAD_LEN + AXS_TOUCH_POINT_LEN]{};

  i2c::ErrorCode err = this->write_read(AXS_READ_TOUCHPAD, sizeof(AXS_READ_TOUCHPAD), data, sizeof(data));
  if (err != i2c::ERROR_OK) {
    this->status_set_warning(LOG_STR("I2C communication failed"));
    return;
  }
  this->status_clear_warning();

  /*
  Touch report:
    0 NULL
    1 Number of fingers touched
    2 [High 4 bits: Event]+[Low 4 bits: High 4 bits of Y-coordinate]
    3 Low 8 bits of Y-coordinate
    4 [High 4 bits: NULL]+[Low 4 bits: High 4 bits of X-coordinate]
    5 Low 8 bits of X-coordinate
    6 NULL
    7 NULL
  */
  uint8_t fingers = data[1];
  uint8_t event = data[2] >> 4;

  // Report single-finger touch events only. Multi-finger reports are the
  // phantom touches seen when the backlight turns off:
  // https://github.com/Xinyuan-LilyGO/T-Display-S3-Long/issues/30
  if (fingers != 1 || event != AXS_TOUCH_EVENT_TOUCH) {
    return;
  }

  uint16_t x = encode_uint16(data[4] & 0xF, data[5]);
  uint16_t y = this->y_raw_max_ - encode_uint16(data[2] & 0xF, data[3]);
  this->add_raw_touch_position_(0, x, y);
}

void AXS15231Touchscreen::dump_config() {
  ESP_LOGCONFIG(TAG, "AXS15231 Touchscreen:");
  LOG_I2C_DEVICE(this);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  LOG_PIN("  Interrupt Pin: ", this->interrupt_pin_);
  ESP_LOGCONFIG(TAG,
                "  X min: %d\n"
                "  X max: %d\n"
                "  Y min: %d\n"
                "  Y max: %d\n"
                "  Swap X/Y: %s\n"
                "  Invert X: %s\n"
                "  Invert Y: %s",
                this->x_raw_min_, this->x_raw_max_, this->y_raw_min_, this->y_raw_max_, YESNO(this->swap_x_y_),
                YESNO(this->invert_x_), YESNO(this->invert_y_));
}

}  // namespace axs15231
}  // namespace esphome
