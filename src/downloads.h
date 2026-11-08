#pragma once

#include <webkit2/webkit2.h>

void init_downloads();
void on_download_started(WebKitWebContext *context, WebKitDownload *download, gpointer user_data);
