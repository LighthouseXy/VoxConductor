#pragma once
// Copy to device_registry.h. Never commit real tokens. Maximum four entries.
// Local peer implements GET /state and POST /command. Values are booleans.
// Prefer HTTPS. HTTP exposes the bearer token on the LAN; use only a trusted LAN.
static const app_device_t APP_DEVICES[] = {
    {"", "", ""},
    // {"desk_light", "http://192.168.1.50", "replace-with-random-token"},
};
