#pragma once

namespace mailai::error_messages::imap {

inline constexpr char connect_stage[] = "imap.connect";
inline constexpr char snapshot_stage[] = "imap.snapshot";
inline constexpr char fetch_stage[] = "imap.fetch";

inline constexpr char invalid_settings[] = "Invalid IMAP account or connection settings";
inline constexpr char invalid_port_or_tls[] = "Invalid IMAP port or TLS mode";
inline constexpr char invalid_timeout[] = "Invalid IMAP timeout";
inline constexpr char invalid_ca_path[] = "Invalid IMAP CA file path";
inline constexpr char invalid_mailbox_path[] = "Invalid IMAP mailbox path";
inline constexpr char missing_password[] = "Missing IMAP password";

inline constexpr char libcurl_init_failed[] = "Cannot initialize libcurl";
inline constexpr char handle_creation_failed[] = "Cannot create IMAP handle";

inline constexpr char url_creation_failed[] = "Cannot create IMAP URL";
inline constexpr char invalid_server_url[] = "Invalid IMAP server URL";
inline constexpr char url_read_failed[] = "Cannot read IMAP server URL";
inline constexpr char url_set_failed[] = "Cannot set IMAP server URL";

inline constexpr char authentication_tls_failed[] = "Cannot configure IMAP authentication or TLS";
inline constexpr char transfer_options_failed[] = "Cannot configure IMAP transfer options";

inline constexpr char invalid_uid_validity[] = "Invalid IMAP UIDVALIDITY response";
inline constexpr char conflicting_uid_validity[] = "Conflicting IMAP UIDVALIDITY responses";
inline constexpr char invalid_search_uid[] = "Invalid UID in IMAP SEARCH response";
inline constexpr char missing_uid_validity[] = "Missing IMAP UIDVALIDITY response";
inline constexpr char missing_search[] = "Missing IMAP SEARCH response";
inline constexpr char snapshot_options_failed[] = "Cannot configure IMAP snapshot request";
inline constexpr char snapshot_request_failed[] = "IMAP snapshot request failed";
inline constexpr char invalid_internal_date[] = "Invalid IMAP INTERNALDATE";
inline constexpr char invalid_remote_key[] = "Invalid IMAP remote mail key";
inline constexpr char fetch_options_failed[] = "Cannot configure IMAP fetch request";
inline constexpr char fetch_request_failed[] = "IMAP fetch request failed";
inline constexpr char uid_validity_changed[] = "IMAP mailbox UIDVALIDITY has changed";
inline constexpr char remote_mail_gone[] = "IMAP message no longer exists";
inline constexpr char invalid_fetch_response[] = "Invalid IMAP FETCH response";
inline constexpr char fetch_too_large[] = "IMAP message exceeds download size limit";
inline constexpr char fetch_response_too_large[] = "IMAP response exceeds size limit";
inline constexpr char service_unavailable[] = "IMAP service temporarily unavailable";

}  // 命名空间 mailai::error_messages::imap

namespace mailai::error_messages::sync {

inline constexpr char source_stage[] = "sync.source";
inline constexpr char fetch_stage[] = "sync.fetch";
inline constexpr char invalid_mail_source[] = "Missing synchronization mail source";
inline constexpr char unexpected_remote_key[] =
    "Fetched mail does not match the pending remote key";

}  // 命名空间 mailai::error_messages::sync
