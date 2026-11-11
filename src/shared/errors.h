/* v0.19: error codes */
#pragma once

namespace atu {

enum ErrCode {
    OK = 0,
    ERR_GENERIC = 1,
    // Directory errors 100-199
    ERR_CANNOT_CREATE_DIR = 100,
    ERR_DIR_NOT_EMPTY,
    ERR_DIR_NOT_FOUND,
    ERR_CANNOT_REMOVE_DIR,
    // File errors 200-299
    ERR_CANNOT_WRITE_FILE = 200,
    ERR_CANNOT_READ_FILE,
    ERR_FILE_NOT_FOUND,
    ERR_FILE_ALREADY_EXISTS,
    ERR_FILE_TOO_LARGE,
    ERR_FILE_CORRUPT,
    ERR_FILE_LOCKED,
    // Payload errors 300-399
    ERR_PAYLOAD_CORRUPT = 300,
    ERR_PAYLOAD_MAGIC,
    ERR_PAYLOAD_INFLATE,
    ERR_DECRYPT_FAILED,
    ERR_PAYLOAD_TRUNCATED,
    ERR_PAYLOAD_CHECKSUM,
    // Config errors 400-499
    ERR_PARSE_CONFIG = 400,
    ERR_MISSING_FIELD,
    ERR_INVALID_VALUE,
    ERR_UNKNOWN_SECTION,
    ERR_DUPLICATE_KEY,
    // Registry errors 500-599
    ERR_REG_OPEN = 500,
    ERR_REG_WRITE,
    ERR_REG_DELETE,
    ERR_REG_NOT_FOUND,
    // Shortcut errors 600-699
    ERR_SHORTCUT = 600,
    ERR_COINIT,
    ERR_SHELLLINK,
    ERR_PERSISTFILE,
    // Disk errors 700-799
    ERR_DISK_SPACE = 700,
    ERR_DISK_WRITE_PROTECTED,
    ERR_DISK_NOT_READY,
    // Version errors 800-899
    ERR_VERSION_CONFLICT = 800,
    ERR_VERSION_TOO_OLD,
    ERR_VERSION_TOO_NEW,
    // User errors 900-999
    ERR_USER_CANCELLED = 900,
    ERR_USER_REJECTED_LICENSE,
    ERR_USER_NO_DISK,
    // System errors 1000-1099
    ERR_OUT_OF_MEMORY = 1000,
    ERR_OUT_OF_HANDLES,
    ERR_ACCESS_DENIED,
    ERR_INVALID_HANDLE,
};

inline const char* err_str(ErrCode c) {
    switch (c) {
        case OK: return "ok";
        case ERR_GENERIC: return "generic error";
        case ERR_CANNOT_CREATE_DIR: return "cannot create directory";
        case ERR_CANNOT_WRITE_FILE: return "cannot write file";
        case ERR_CANNOT_READ_FILE: return "cannot read file";
        case ERR_FILE_NOT_FOUND: return "file not found";
        case ERR_PAYLOAD_CORRUPT: return "payload corrupt";
        case ERR_PAYLOAD_MAGIC: return "bad payload magic";
        case ERR_PAYLOAD_INFLATE: return "decompression failed";
        case ERR_DECRYPT_FAILED: return "decryption failed";
        case ERR_PARSE_CONFIG: return "config parse error";
        case ERR_MISSING_FIELD: return "missing required field";
        case ERR_INVALID_VALUE: return "invalid value";
        case ERR_REG_OPEN: return "registry open failed";
        case ERR_REG_WRITE: return "registry write failed";
        case ERR_COINIT: return "COM init failed";
        case ERR_SHELLLINK: return "shortcut creation failed";
        case ERR_DISK_SPACE: return "insufficient disk space";
        case ERR_VERSION_CONFLICT: return "version conflict";
        case ERR_USER_CANCELLED: return "user cancelled";
        default: return "unknown";
    }
}

} // namespace atu
