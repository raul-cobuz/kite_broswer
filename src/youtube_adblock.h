#pragma once

// Adds or removes the YouTube-specific ad-block script. YouTube serves video
// ads from its own domains and injects them through player JSON responses, so
// URL filters alone cannot remove them.
void youtube_adblock_set_enabled(bool enabled);
