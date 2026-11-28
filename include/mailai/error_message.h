#pragma once

namespace mailai::error_message::common {

inline constexpr char unknown_account[] = "Unknown account";
inline constexpr char mail_account_missing[] = "Mail account does not exist";
inline constexpr char invalid_mail_id[] = "Mail ID must be greater than zero";
inline constexpr char mail_not_found[] = "Mail not found";

}  // namespace mailai::error_message::common

namespace mailai::error_message::cli {

inline constexpr char inbox_usage[] = "Usage: inbox [--account <id|all>]";
inline constexpr char account_required[] = "--account requires an account ID or all";
inline constexpr char show_usage[] = "Usage: show <mail-id>";
inline constexpr char invalid_id[] =
    "Mail ID must be a positive decimal integer in the unsigned 64-bit range";
inline constexpr char demo_command_required[] = "--demo requires inbox, find or show";
inline constexpr char unknown_command[] = "Unknown command; use --help for usage";
inline constexpr char demo_sync_forbidden[] = "Synchronization is not available in demo mode";
inline constexpr char sync_usage[] = "Usage: sync [--history <count> | --all]";
inline constexpr char invalid_history_count[] = "History count must be a positive decimal integer";
inline constexpr char find_usage[] = "Usage: find <keyword>";
inline constexpr char invalid_command[] = "Unknown command";
inline constexpr char execution_failed[] = "Application execution failed";

}  // namespace mailai::error_message::cli

namespace mailai::error_message::login_cli {

inline constexpr char login_usage[] = "Usage: login [--advanced]";
inline constexpr char logout_usage[] = "Usage: logout";
inline constexpr char demo_forbidden[] = "Login and logout are not available in demo mode";
inline constexpr char terminal_required[] = "Login requires an interactive macOS terminal";
inline constexpr char input_cancelled[] = "Login input was cancelled";
inline constexpr char invalid_email[] = "Email must contain a nonempty local part and domain";
inline constexpr char invalid_host[] = "IMAP host must be a hostname or IP address, not a URL";
inline constexpr char unavailable[] = "Login service unavailable";
inline constexpr char logout_unavailable[] = "Logout service unavailable";
inline constexpr char password_missing[] = "Password entry was cancelled, empty or too long";
inline constexpr char login_failed[] = "Login failed";

}  // namespace mailai::error_message::login_cli

namespace mailai::error_message::config {

inline constexpr char data_dir_missing[] = "Cannot determine data directory";
inline constexpr char required_setting[] = "Missing required setting: ";
inline constexpr char invalid_tls[] = "MAILAI_IMAP_TLS must be implicit or starttls";
inline constexpr char invalid_port[] = "MAILAI_IMAP_PORT must be a decimal integer from 1 to 65535";
inline constexpr char invalid_credential_reference[] =
    "MAILAI_PASSWORD_ENV must contain an environment variable name, not an assignment";

}  // namespace mailai::error_message::config

namespace mailai::error_message::login {

inline constexpr char login_required[] =
    "No saved mailbox configuration; run mail_assistant login first";
inline constexpr char invalid_login[] = "Mailbox settings or password are invalid";
inline constexpr char absolute_data_dir_failed[] = "Cannot resolve an absolute cache directory";
inline constexpr char rollback_failed[] =
    "Login could not be saved and credential rollback failed; run login again";
inline constexpr char invalid_cache_directory[] = "The selected mail cache directory is not usable";
inline constexpr char canonical_profile_failed[] = "Cannot resolve the saved login directory";
inline constexpr char absolute_ca_failed[] = "Cannot resolve an absolute CA certificate path";
inline constexpr char invalid_service[] = "Login service is not available";

}  // namespace mailai::error_message::login

namespace mailai::error_message::storage {

inline constexpr char inspect_cache_failed[] = "Cannot inspect mail cache";
inline constexpr char create_directory_failed[] = "Cannot create data directory";
inline constexpr char unsafe_cache_directory[] =
    "Mail cache directory must be owned by the current user and not writable by others";
inline constexpr char unsafe_cache_file[] =
    "Mail cache files must be regular files owned by the current user, without hard links";
inline constexpr char protect_cache_failed[] = "Cannot protect mail cache file permissions";
inline constexpr char required_account_settings[] = "Required account settings are missing";
inline constexpr char invalid_tls_mode[] = "Invalid TLS mode";
inline constexpr char account_identity_mismatch[] =
    "Account IMAP identity does not match saved settings";
inline constexpr char invalid_lock_settings[] = "Invalid data-directory lock settings";
inline constexpr char lock_open_failed[] = "Cannot open data-directory lock: ";
inline constexpr char lock_acquire_failed[] = "Cannot acquire data-directory lock: ";
inline constexpr char directory_busy[] = "Data directory is busy";
inline constexpr char invalid_pending_job[] = "Invalid stored pending job";
inline constexpr char account_folder_required[] = "Account and folder must not be empty";
inline constexpr char invalid_discovery_settings[] = "Invalid discovery settings";
inline constexpr char zero_uid[] = "Messages UIDs must not be zero";
inline constexpr char remote_key_mismatch[] = "Remote key does not match the current mailbox state";
inline constexpr char invalid_remote_key[] = "Invalid remote mail key";
inline constexpr char invalid_parse_failure[] = "Invalid parsing failure";
inline constexpr char too_many_attachments[] = "Too many attachments";
inline constexpr char attachment_size_overflow[] =
    "Attachment size exceeds the SQLite integer range";
inline constexpr char saved_mail_missing[] = "Saved mail was not found";
inline constexpr char invalid_stored_id[] = "Invalid stored mail ID";
inline constexpr char invalid_failure_report[] = "Invalid failure report";
inline constexpr char pending_job_missing[] = "Pending job was not found";
inline constexpr char invalid_parse_state[] = "Invalid stored parse state";
inline constexpr char invalid_stored_key[] = "Invalid stored remote mail key";
inline constexpr char invalid_sync_state[] = "Invalid stored sync state";
inline constexpr char statement_too_large[] = "SQL statement exceeds SQLite size limit";
inline constexpr char empty_statement[] = "SQL statement is empty";
inline constexpr char parameter_count_mismatch[] = "Values do not match SQL parameter count";
inline constexpr char expected_integer_result[] = "Expected one integer result";
inline constexpr char foreign_keys_failed[] = "SQLite foreign keys could not be enabled";
inline constexpr char missing_journal_mode[] = "SQLite did not return a journal mode";
inline constexpr char wal_failed[] = "SQLite WAL mode could not be enabled";
inline constexpr char expected_integer_column[] = "Expected an INTEGER SQL value at column ";
inline constexpr char expected_nonnegative_column[] =
    "Expected a nonnegative SQL integer at column ";
inline constexpr char uid_range_exceeded[] = "SQL integer exceeds the UID range at column ";
inline constexpr char expected_boolean_column[] = "Expected a SQL boolean at column ";
inline constexpr char expected_text_column[] = "Expected a TEXT SQL value at column ";
inline constexpr char text_read_failed[] = "Could not read a TEXT SQL value";
inline constexpr char expected_blob_column[] = "Expected a BLOB SQL value at column ";
inline constexpr char blob_read_failed[] = "Could not read a BLOB SQL value";
inline constexpr char unexpected_result_row[] = "Unexpected additional SQL result row";
inline constexpr char batch_parameter_count_mismatch[] =
    "Batch row does not match SQL parameter count";
inline constexpr char unexpected_column_count[] = "Unexpected SQL result column count";
inline constexpr char missing_schema_object[] = "Missing schema object: ";
inline constexpr char unexpected_schema_object[] = "Unexpected schema object: ";
inline constexpr char schema_mismatch[] = "Schema definition mismatch: ";
inline constexpr char unsupported_database_version[] = "Unsupported database version: ";
inline constexpr char nonempty_unversioned_database[] =
    "Cannot initialize a non-empty unversioned database";
inline constexpr char readonly_initialization[] =
    "Database requires initialization with write access";

}  // namespace mailai::error_message::storage

namespace mailai::error_message::mime {

inline constexpr char missing_part[] = "Missing MIME part";
inline constexpr char depth_limit[] = "MIME depth limit exceeded";
inline constexpr char part_count_limit[] = "MIME part count limit exceeded";
inline constexpr char missing_embedded_message[] = "Missing embedded message";
inline constexpr char unsupported_charset[] = "Unknown or unsupported text charset";
inline constexpr char invalid_utf8_body[] = "Cannot read a UTF-8 body";
inline constexpr char html_too_large[] = "HTML is too large";
inline constexpr char html_parse_failed[] = "Cannot parse HTML";
inline constexpr char missing_html_root[] = "HTML has no root element";
inline constexpr char invalid_attachment_filename[] = "Attachment filename is not UTF-8";
inline constexpr char missing_attachment_content_type[] = "Missing attachment content type";
inline constexpr char attachment_media_type_failed[] = "Cannot read attachment media type";
inline constexpr char attachment_content_failed[] = "Cannot read attachment content";
inline constexpr char attachment_decode_failed[] = "Cannot decode attachment content";
inline constexpr char unsupported_attachment[] = "Unsupported attachment type";
inline constexpr char empty_message[] = "Empty message";
inline constexpr char message_size_limit[] = "Message size limit exceeded";
inline constexpr char message_parse_failed[] = "Cannot parse message";
inline constexpr char missing_text_body[] = "Expected a text/plain or text/html message";

}  // namespace mailai::error_message::mime

namespace mailai::error_message::imap {

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

}  // namespace mailai::error_message::imap

namespace mailai::error_message::sync {

inline constexpr char source_stage[] = "sync.source";
inline constexpr char fetch_stage[] = "sync.fetch";
inline constexpr char invalid_mail_source[] = "Missing synchronization mail source";
inline constexpr char unexpected_remote_key[] =
    "Fetched mail does not match the pending remote key";

}  // namespace mailai::error_message::sync

namespace mailai::error_message::live_probe {

inline constexpr char enable_required[] =
    "Live mailbox probe is disabled; set MAILAI_LIVE_TESTS=1 to enable it";
inline constexpr char failed[] = "Live mailbox probe failed";

}  // namespace mailai::error_message::live_probe

namespace mailai::error_message::profile {

inline constexpr char directory_missing[] = "Cannot determine login profile directory";
inline constexpr char directory_create_failed[] = "Cannot create login profile directory";
inline constexpr char directory_unsafe[] = "Login profile directory is inaccessible or unsafe";
inline constexpr char file_unsafe[] = "Login profile file is inaccessible or unsafe";
inline constexpr char malformed[] = "Malformed login profile";
inline constexpr char invalid_value[] = "Invalid login profile value";
inline constexpr char absolute_data_dir_failed[] =
    "Cannot resolve an absolute login data directory";
inline constexpr char profile_too_large[] = "Login profile exceeds size limit";
inline constexpr char read_failed[] = "Cannot read login profile";
inline constexpr char write_failed[] = "Cannot write login profile";

}  // namespace mailai::error_message::profile

namespace mailai::error_message::keychain {

inline constexpr char unsupported[] = "Keychain credentials require macOS";
inline constexpr char invalid_account[] = "Invalid Keychain account key";
inline constexpr char invalid_password[] = "Invalid IMAP password";
inline constexpr char missing_password[] = "No saved IMAP password; run login first";
inline constexpr char invalid_saved_password[] = "Saved IMAP password is invalid; run login again";
inline constexpr char query_failed[] = "Cannot prepare Keychain query";
inline constexpr char read_failed[] = "Cannot read IMAP password from Keychain";
inline constexpr char write_failed[] = "Cannot save IMAP password in Keychain";
inline constexpr char erase_failed[] = "Cannot remove IMAP password from Keychain";

}  // namespace mailai::error_message::keychain
