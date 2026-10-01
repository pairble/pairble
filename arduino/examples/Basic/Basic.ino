// pairble, the smallest integration.
//
// With a network stored the board connects to it; without one it opens setup
// and prints the URL to put in a QR code. Switch the board on three times in
// a row, each time for less than 5 seconds, to open setup again.

#include <pairble.h>

static void onPairble(pairble_event_t event, pairble_fail_reason_t reason, void *)
{
  switch (event)
  {
    case PAIRBLE_EVENT_STARTED:
      Serial.printf("Setup open - scan the QR code: %s\n", pairble_setup_url());
      break;
    case PAIRBLE_EVENT_FAILED:
      Serial.printf("That did not work (%d), the phone can try again\n", reason);
      break;
    case PAIRBLE_EVENT_ONLINE:
      Serial.println("Online");
      break;
    case PAIRBLE_EVENT_OFFLINE:
      Serial.printf("Offline (%d)\n", reason);
      break;
    default:
      break;
  }
}

void setup()
{
  Serial.begin(115200);
  pairble_config_t config = PAIRBLE_CONFIG_DEFAULT();
  config.event_cb = onPairble;
  ESP_ERROR_CHECK(pairble_start(&config));
  Serial.printf("Device %s\n", pairble_device_name());
}

void loop()
{
  // Nothing to do here: pairble runs in the background. Use Arduino's
  // networking (HTTPClient, WiFiClient...) once it reports Online.
  delay(1000);
}
