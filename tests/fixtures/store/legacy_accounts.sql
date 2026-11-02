CREATE TABLE IF NOT EXISTS accounts (
                id TEXT PRIMARY KEY NOT NULL,
                display_name TEXT NOT NULL,
                address TEXT NOT NULL,
                imap_host TEXT NOT NULL,
                imap_port INTEGER NOT NULL,
                imap_username TEXT NOT NULL,
                credential_env TEXT NOT NULL,
                tls_mode TEXT NOT NULL
            );
