#pragma once

// StructRegistry - Runtime struct definition registry.
// Loads from built-in D2 structs + structs.json, supports runtime additions.
// Used by MCP tools to read typed memory and by the Python explorer.

#include <windows.h>
#include <string>
#include <vector>
#include <map>

namespace StructRegistry {

    enum FieldType {
        FIELD_BYTE = 0,
        FIELD_WORD,
        FIELD_DWORD,
        FIELD_INT,
        FIELD_FLOAT,
        FIELD_POINTER,      // pointer to another struct
        FIELD_STRING,       // char* or char[]
        FIELD_WSTRING,      // wchar_t*
        FIELD_ARRAY,        // fixed-size array
        FIELD_PADDING,      // unknown/padding bytes
    };

    struct FieldDef {
        std::string name;
        int offset;
        FieldType type;
        int size;                   // in bytes
        std::string pointsTo;      // struct name if FIELD_POINTER
        int arrayCount;            // element count if FIELD_ARRAY
        std::string comment;
    };

    struct StructDef {
        std::string name;
        int size;                   // total struct size
        std::string source;        // "builtin", "json", "discovered"
        std::vector<FieldDef> fields;
    };

    // Initialize with built-in D2 struct definitions
    void Init();

    // Load additional structs from a JSON file (merges with existing)
    int LoadFromFile(const std::string& path);

    // Save all structs to a JSON file
    bool SaveToFile(const std::string& path);

    // Get a struct definition by name
    const StructDef* GetStruct(const std::string& name);

    // Add or update a struct definition
    void AddStruct(const StructDef& def);

    // List all known struct names
    std::vector<std::string> ListStructs();

    // Get all struct definitions
    std::map<std::string, StructDef> GetAllStructs();

    // Get struct count
    int GetStructCount();
}
