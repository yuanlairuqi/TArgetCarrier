/* Copy to tc_pairing.local.h; use actual peer STA MAC, real channel and private
 * random keys. Never commit local header or paired build artifacts.
 * Each endpoint has a different TC_PEER_MAC. No synthetic pairing in builds. */
#define TC_RADIO_ENABLED 0
#define TC_WIFI_CHANNEL 6
#define TC_PEER_MAC {0, 0, 0, 0, 0, 0}
#define TC_PMK {0}      /* 16 actual private bytes */
#define TC_LMK {0}      /* 16 actual private bytes */
#define TC_AUTH_KEY {0} /* 32 actual private bytes */
