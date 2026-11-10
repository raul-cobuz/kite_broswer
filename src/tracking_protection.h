#pragma once

// Installs the link-cleaning user script when tracking-parameter stripping is
// enabled. Call after the ad-block content manager exists.
void tracking_protection_init();

// Adds or removes the user script. Already-loaded pages keep the previous
// behavior until they are reloaded.
void tracking_protection_set_enabled(bool enabled);
