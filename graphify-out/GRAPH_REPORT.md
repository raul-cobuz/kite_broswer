# Graph Report - BROWSER  (2026-11-17)

## Corpus Check
- 66 files · ~53,053 words
- Verdict: corpus is large enough that graph structure adds value.
- Unclassified: 5 file(s) not represented in the graph (top: (none) 4, .in 1)

## Summary
- 1005 nodes · 2941 edges · 49 communities (44 shown, 5 thin omitted)
- Extraction: 83% EXTRACTED · 17% INFERRED · 0% AMBIGUOUS · INFERRED: 504 edges (avg confidence: 0.85)
- Token cost: 0 input · 0 output

## Community Hubs (Navigation)
- App bootstrap and contexts
- Internal kite pages
- History SQLite layer
- Command palette widget
- Omnibox ranking
- Tab lifecycle
- Settings and zoom
- URL and search utilities
- Core module headers
- Ad blocking engine
- Split view management
- Downloads UI
- Extension lifecycle
- Sync JSON serialization
- App accelerators
- Extension background hosts
- Browser settings API
- Entry point and UI headers
- Browser window chrome
- Extension manifest model
- chrome.* API handlers
- Extension message routing
- Content script injection
- Privacy site data
- Tab creation and menus
- Find bar
- CI build workflow
- README feature docs
- Screenshot tool
- Filter generator and tests
- Session persistence
- Error pages
- Shortcuts and actions
- Permission prompts
- Tab load and bookmark UI
- Dark mode
- Tab context menu
- Privacy and sync controls
- Tab info struct
- Build metadata and icon
- Window and completion events
- New-tab links
- Registered message handler
- Tracking protection
- Adblock rule tests
- Core test build
- Quit handling
- Adblock documentation
- Dark mode documentation

## God Nodes (most connected - your core abstractions)
1. `get_active_webview()` - 49 edges
2. `main()` - 48 edges
3. `ExtensionInfo` - 35 edges
4. `newtab_page()` - 33 edges
5. `settings_page()` - 32 edges
6. `main()` - 25 edges
7. `save_settings()` - 25 edges
8. `create_browser_window()` - 23 edges
9. `sync_merge_file()` - 23 edges
10. `on_load_changed()` - 23 edges

## Surprising Connections (you probably didn't know these)
- `Kite application icon` --conceptually_related_to--> `Default browser registration`  [INFERRED]
  data/kite.svg → README.md
- `main()` --calls--> `adblock_exception_rules_json()`  [INFERRED]
  tests/test_adblock_rules.cpp → src/adblock_rules.cpp
- `main()` --calls--> `history_is_ready()`  [INFERRED]
  tests/test_history.cpp → src/history.cpp
- `main()` --calls--> `omnibox_suggestions()`  [INFERRED]
  tests/test_omnibox.cpp → src/omnibox.cpp
- `main()` --calls--> `profile_init()`  [INFERRED]
  tests/test_sync.cpp → src/profile.cpp

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **Kite CMake build graph** — cmakelists_kite_core, cmakelists_kite_ui_sources, cmakelists_kite_add_browser, cmakelists_mybrowser, cmakelists_mybrowser_debug, cmakelists_build_testing [EXTRACTED 1.00]
- **Kite privacy and data-isolation model** — readme_profiles, readme_permissions, readme_adblock_blocking, readme_settings_ini, readme_kite_db, readme_session_restore [INFERRED 0.85]
- **Kite local-first search surfaces** — readme_omnibox, readme_url_normalization, readme_omnibox_bangs, readme_command_palette, readme_kite_db [INFERRED 0.85]

## Communities (49 total, 5 thin omitted)

### Community 0 - "App bootstrap and contexts"
Cohesion: 0.07
Nodes (69): app_context_init(), app_webkit_context(), avatar_color_class(), create_browser_window(), setup_omnibox_completion(), extensions_init(), extensions_register_scheme(), init_kite_pages() (+61 more)

### Community 1 - "Internal kite pages"
Cohesion: 0.13
Nodes (48): avatar_css_class(), bookmarks_page(), css_url_value(), display_host(), entry_row(), escape_query_value(), extension_action_href(), extension_notice_html() (+40 more)

### Community 2 - "History SQLite layer"
Cohesion: 0.09
Nodes (41): bookmark_add(), bookmark_exists(), bookmark_remove(), bookmark_toggle(), bookmarks_list(), capped_limit(), exec_sql(), history_clear() (+33 more)

### Community 3 - "Command palette widget"
Cohesion: 0.08
Nodes (31): action_items(), activate_selected(), add_action(), clear_list(), close_palette(), CommandItem, detail, id (+23 more)

### Community 4 - "Omnibox ranking"
Cohesion: 0.10
Nodes (25): add_candidate(), builtin_pages(), Candidate, score, suggestion, contains(), kind_bonus(), lower() (+17 more)

### Community 5 - "Tab lifecycle"
Cohesion: 0.14
Nodes (23): close_tab(), GoogleFallbackRequest, notebook_views(), on_decide_policy(), on_favicon_changed(), on_tab_close_clicked(), remember_closed_tab(), schedule_google_fallback() (+15 more)

### Community 6 - "Settings and zoom"
Cohesion: 0.15
Nodes (24): boolean_setting(), clamp_zoom(), compare_hosts(), default_download_directory(), get_permission_decision(), load_newtab_links(), load_settings(), normalize_host() (+16 more)

### Community 7 - "URL and search utilities"
Cohesion: 0.20
Nodes (23): decode_form_value(), escape_query(), google_challenge_query(), google_search_url(), has_known_scheme(), hex_value(), is_google_challenge_url(), is_google_domain() (+15 more)

### Community 9 - "Ad blocking engine"
Cohesion: 0.13
Nodes (14): adblock_reload_exceptions(), adblock_set_enabled(), apply_base_filter(), find_filters_json_path(), init_adblocker(), is_youtube_host(), on_base_filter_saved(), on_exception_saved() (+6 more)

### Community 10 - "Split view management"
Cohesion: 0.19
Nodes (22): activate_webview(), cycle_active_tab(), deactivate_split_view(), is_startup_newtab(), move_all_split_pages_to_main(), move_notebook_page(), move_tab_to_split(), notebook_current_view() (+14 more)

### Community 11 - "Downloads UI"
Cohesion: 0.15
Nodes (15): create_download_row(), create_downloads_button(), download_item_free(), download_suggested_filename(), DownloadItem, init_downloads(), on_cancel_clicked(), on_clear_clicked() (+7 more)

### Community 12 - "Extension lifecycle"
Cohesion: 0.20
Nodes (23): clear_background_hosts(), copy_directory(), destroy_background_host(), extension_has_permission(), extension_path(), extensions_directory(), extensions_install_from_folder(), extensions_reload() (+15 more)

### Community 13 - "Sync JSON serialization"
Cohesion: 0.14
Nodes (17): settings_set_dark_mode(), settings_set_restore_session(), append_bool_field(), append_escaped(), append_string_field(), json_bool(), json_number(), json_string() (+9 more)

### Community 14 - "App accelerators"
Cohesion: 0.17
Nodes (16): on_accel_palette(), on_accel_print(), on_accel_screenshot_full(), on_accel_screenshot_visible(), on_back_clicked(), on_extensions_clicked(), on_forward_clicked(), on_home_clicked() (+8 more)

### Community 15 - "Extension background hosts"
Cohesion: 0.10
Nodes (20): BackgroundHost, announced, extension_id, handler_name, install_reason, loaded, manager, queued (+12 more)

### Community 16 - "Browser settings API"
Cohesion: 0.17
Nodes (23): settings_page(), save_settings(), settings_adblock_exception_hosts(), settings_download_directory(), settings_google_fallback(), settings_homepage(), settings_newtab_background(), settings_newtab_clock() (+15 more)

### Community 18 - "Browser window chrome"
Cohesion: 0.15
Nodes (13): clear_omnibox_completions(), create_kite_logo_icon_list(), create_kite_logo_pixbuf(), find_kite_svg_path(), navigate_url_entry(), on_search_notice_response(), on_url_activate(), refresh_search_placeholder() (+5 more)

### Community 19 - "Extension manifest model"
Cohesion: 0.11
Nodes (21): background_url(), build_shim(), create_background_host(), ExtensionInfo, background_page, background_scripts, description, enabled (+13 more)

### Community 20 - "chrome.* API handlers"
Cohesion: 0.23
Nodes (20): copy_keys(), handle_notifications(), handle_storage(), handle_tabs(), js_string(), json_bool(), json_escape(), json_int() (+12 more)

### Community 21 - "Extension message routing"
Cohesion: 0.19
Nodes (16): complete_pending(), dispatch_to_tab(), error_reply(), handle_api_message(), new_request_id(), on_pending_timeout(), on_script_message(), pending_reply_free() (+8 more)

### Community 22 - "Content script injection"
Cohesion: 0.13
Nodes (18): adblock_content_manager(), append_allowed_patterns(), clear_injections(), ContentScriptSpec, all_frames, css, js, matches (+10 more)

### Community 23 - "Privacy site data"
Cohesion: 0.18
Nodes (13): name_matches_site(), on_clear_on_exit_finished(), on_clear_on_exit_toggled(), on_clear_site_data_clicked(), on_global_switch_notify(), on_site_data_fetched(), on_site_data_removed(), on_site_switch_notify() (+5 more)

### Community 24 - "Tab creation and menus"
Cohesion: 0.16
Nodes (12): add_menu_item(), apply_webview_settings(), create_incognito_tab(), create_tab(), create_tab_view(), on_create_tab(), on_notebook_button_press(), on_tab_box_button_press() (+4 more)

### Community 25 - "Find bar"
Cohesion: 0.22
Nodes (11): get_active_webview(), close_find_bar(), create_find_bar(), on_find_activate(), on_find_changed(), on_find_close(), on_find_failed(), on_find_found() (+3 more)

### Community 26 - "CI build workflow"
Cohesion: 0.12
Nodes (16): runs-on, steps, jobs, build, name, on, pull_request, push (+8 more)

### Community 27 - "README feature docs"
Cohesion: 0.13
Nodes (17): Command palette (Ctrl+K), Custom extension runtime, kite:// internal pages, kite.db SQLite store, kite-sync.json, Extension manifest.json, Omnibox suggestions and completion, Omnibox bangs (!yt, !gh, !w, !r) (+9 more)

### Community 28 - "Screenshot tool"
Cohesion: 0.12
Nodes (8): on_snapshot_ready(), safe_filename(), screenshot_directory(), ScreenshotRequest, full_page, path, view, show_result_dialog()

### Community 29 - "Filter generator and tests"
Cohesion: 0.23
Nodes (7): find_block_rule(), main(), build_rules(), clean_domains(), domain_url_filter(), generate(), main()

### Community 30 - "Session persistence"
Cohesion: 0.20
Nodes (10): session_clear(), session_load(), session_path(), session_save(), SessionState, active_index, pinned, urls (+2 more)

### Community 31 - "Error pages"
Cohesion: 0.29
Nodes (5): on_load_failed(), on_load_failed_with_tls_errors(), on_web_process_terminated(), set_tab_error_label(), show_error_page()

### Community 32 - "Shortcuts and actions"
Cohesion: 0.24
Nodes (12): close_current_tab(), focus_url_bar(), on_key_press(), on_new_tab_clicked(), execute_item(), open_url(), connect_find_handlers(), run_find() (+4 more)

### Community 33 - "Permission prompts"
Cohesion: 0.23
Nodes (5): on_permission_dialog_response(), on_permission_request(), permission_description(), permission_origin(), PermissionPrompt

### Community 34 - "Tab load and bookmark UI"
Cohesion: 0.26
Nodes (10): can_bookmark_uri(), on_bookmark_toggled(), on_url_entry_changed(), set_active_zoom(), update_bookmark_button(), webview_is_incognito(), apply_site_zoom(), is_trackable_url() (+2 more)

### Community 35 - "Dark mode"
Cohesion: 0.26
Nodes (8): apply_dark_mode(), init_dark_mode(), on_dark_mode_toggled(), set_dark_mode_script(), set_webview_dark_mode(), sync_native_theme(), toggle_dark_mode(), uri_is_internal()

### Community 36 - "Tab context menu"
Cohesion: 0.26
Nodes (9): menu_unref_view(), on_menu_close_others(), on_menu_close_right(), on_menu_copy_url(), on_menu_duplicate(), on_menu_move_split(), on_menu_mute(), on_menu_pin() (+1 more)

### Community 37 - "Privacy and sync controls"
Cohesion: 0.23
Nodes (9): create_privacy_button(), make_switch_row(), update_privacy_button(), view_host(), settings_block_ads(), settings_clear_on_exit(), settings_restore_session(), settings_strip_tracking_params() (+1 more)

### Community 38 - "Tab info struct"
Cohesion: 0.20
Nodes (9): KiteTabInfo, active, id, incognito, index, muted, pinned, title (+1 more)

### Community 39 - "Build metadata and icon"
Cohesion: 0.25
Nodes (9): kite_add_browser(), KITE_UI_SOURCES list, mybrowser executable target, mybrowser-debug executable target, Kite application icon, Faceted kite diamond glyph, Make + CMake build workflow, Default browser registration (+1 more)

### Community 40 - "Window and completion events"
Cohesion: 0.28
Nodes (4): center_split_pane(), completion_match_always(), on_completion_match(), on_window_delete()

### Community 41 - "New-tab links"
Cohesion: 0.33
Nodes (5): NewTabLink, title, url, settings_newtab_links(), settings_set_newtab_links()

### Community 42 - "Registered message handler"
Cohesion: 0.40
Nodes (4): RegisteredHandler, name, signal_id, world

### Community 43 - "Tracking protection"
Cohesion: 0.70
Nodes (4): add_tracking_script(), remove_tracking_script(), tracking_protection_init(), tracking_protection_set_enabled()

### Community 44 - "Adblock rule tests"
Cohesion: 0.70
Nodes (3): expect_eq(), main(), one_rule()

### Community 45 - "Core test build"
Cohesion: 0.67
Nodes (3): adblock_filters Python test, BUILD_TESTING test suite, kite_core static library

## Knowledge Gaps
- **115 isolated node(s):** `Action`, `Url`, `Tab`, `kind`, `id` (+110 more)
  These have ≤1 connection - possible missing edges or undocumented components. (Counts symbols only; 233 node(s) total have ≤1 connection when file, concept and rationale nodes are included.)
- **5 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `get_active_webview()` connect `Find bar` to `Shortcuts and actions`, `App bootstrap and contexts`, `Tab load and bookmark UI`, `Command palette widget`, `Privacy and sync controls`, `Tab lifecycle`, `Window and completion events`, `Split view management`, `App accelerators`, `Browser window chrome`, `chrome.* API handlers`, `Privacy site data`, `Error pages`?**
  _High betweenness centrality (0.058) - this node is a cross-community bridge._
- **Are the 23 inferred relationships involving `get_active_webview()` (e.g. with `execute_item()` and `open_url()`) actually correct?**
  _`get_active_webview()` has 23 INFERRED edges - model-reasoned connections that need verification._
- **What connects `Action`, `Url`, `Tab` to the rest of the system?**
  _115 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `App bootstrap and contexts` be split into smaller, more focused modules?**
  _Cohesion score 0.06817513958272113 - nodes in this community are weakly interconnected._
- **Why does `ExtensionInfo` connect `Extension manifest model` to `Core module headers`, `Extension lifecycle`, `Extension background hosts`, `chrome.* API handlers`, `Extension message routing`, `Content script injection`?**
  _High betweenness centrality (0.047) - this node is a cross-community bridge._
- **Are the 45 inferred relationships involving `main()` (e.g. with `get_permission_decision()` and `load_settings()`) actually correct?**
  _`main()` has 45 INFERRED edges - model-reasoned connections that need verification._
- **Should `Internal kite pages` be split into smaller, more focused modules?**
  _Cohesion score 0.12794612794612795 - nodes in this community are weakly interconnected._