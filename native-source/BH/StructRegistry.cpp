#include "StructRegistry.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <mutex>

using rjson = nlohmann::json;

namespace {
    std::mutex g_mutex;
    std::map<std::string, StructRegistry::StructDef> g_structs;

    using namespace StructRegistry;

    // Helper to add a field definition
    FieldDef F(const char* name, int offset, FieldType type, int size,
               const char* pointsTo = "", int arrayCount = 0, const char* comment = "") {
        return { name, offset, type, size, pointsTo, arrayCount, comment };
    }

    void RegisterBuiltinStructs() {
        // UnitAny — the core game unit struct
        {
            StructDef s = { "UnitAny", 0xEC, "builtin", {
                F("dwType",         0x00, FIELD_DWORD, 4, "", 0, "0=Player,1=Monster,2=Object,3=Missile,4=Item,5=Tile"),
                F("dwTxtFileNo",    0x04, FIELD_DWORD, 4, "", 0, "class/item/monster ID"),
                F("_1",             0x08, FIELD_PADDING, 4),
                F("dwUnitId",       0x0C, FIELD_DWORD, 4, "", 0, "unique unit ID"),
                F("dwMode",         0x10, FIELD_DWORD, 4, "", 0, "animation mode (0=death,12=dead)"),
                F("pData",          0x14, FIELD_POINTER, 4, "", 0, "PlayerData/ItemData/MonsterData/ObjectData"),
                F("dwAct",          0x18, FIELD_DWORD, 4),
                F("pAct",           0x1C, FIELD_POINTER, 4, "Act"),
                F("dwSeed",         0x20, FIELD_ARRAY, 8, "", 2),
                F("_2",             0x28, FIELD_PADDING, 4),
                F("pPath",          0x2C, FIELD_POINTER, 4, "Path"),
                F("_3",             0x30, FIELD_PADDING, 44),
                F("pStats",         0x5C, FIELD_POINTER, 4, "StatList"),
                F("pInventory",     0x60, FIELD_POINTER, 4, "Inventory"),
                F("_4",             0x64, FIELD_PADDING, 40),
                F("wX",             0x8C, FIELD_WORD, 2),
                F("wY",             0x8E, FIELD_WORD, 2),
                F("_5",             0x90, FIELD_PADDING, 4),
                F("dwOwnerType",    0x94, FIELD_DWORD, 4),
                F("dwOwnerId",      0x98, FIELD_DWORD, 4),
                F("_6",             0x9C, FIELD_PADDING, 12),
                F("pInfo",          0xA8, FIELD_POINTER, 4, "Info", 0, "skills"),
                F("_7",             0xAC, FIELD_PADDING, 52),
                F("pChangedNext",   0xE0, FIELD_POINTER, 4, "UnitAny"),
                F("pRoomNext",      0xE4, FIELD_POINTER, 4, "UnitAny"),
                F("pListNext",      0xE8, FIELD_POINTER, 4, "UnitAny"),
            }};
            g_structs["UnitAny"] = s;
        }

        // Path
        {
            StructDef s = { "Path", 0x64, "builtin", {
                F("xOffset",    0x00, FIELD_WORD, 2),
                F("xPos",       0x02, FIELD_WORD, 2),
                F("yOffset",    0x04, FIELD_WORD, 2),
                F("yPos",       0x06, FIELD_WORD, 2),
                F("_1",         0x08, FIELD_PADDING, 8),
                F("xTarget",    0x10, FIELD_WORD, 2),
                F("yTarget",    0x12, FIELD_WORD, 2),
                F("_2",         0x14, FIELD_PADDING, 8),
                F("pRoom1",     0x1C, FIELD_POINTER, 4, "Room1"),
                F("pRoomUnk",   0x20, FIELD_POINTER, 4, "Room1"),
                F("_3",         0x24, FIELD_PADDING, 12),
                F("pUnit",      0x30, FIELD_POINTER, 4, "UnitAny"),
            }};
            g_structs["Path"] = s;
        }

        // Room1
        {
            StructDef s = { "Room1", 0x80, "builtin", {
                F("_1",         0x00, FIELD_PADDING, 4),
                F("pRoomsNear", 0x04, FIELD_POINTER, 4),
                F("_2",         0x08, FIELD_PADDING, 16),
                F("pRoom2",     0x18, FIELD_POINTER, 4, "Room2"),
                F("_3",         0x1C, FIELD_PADDING, 20),
                F("pUnitFirst", 0x30, FIELD_POINTER, 4, "UnitAny", 0, "first unit in room"),
                F("_4",         0x34, FIELD_PADDING, 44),
                F("pRoomNext",  0x60, FIELD_POINTER, 4, "Room1"),
            }};
            g_structs["Room1"] = s;
        }

        // Room2
        {
            StructDef s = { "Room2", 0x50, "builtin", {
                F("_1",         0x00, FIELD_PADDING, 4),
                F("_2",         0x04, FIELD_PADDING, 28),
                F("pLevel",     0x20, FIELD_POINTER, 4, "Level"),
            }};
            g_structs["Room2"] = s;
        }

        // Level
        {
            StructDef s = { "Level", 0x230, "builtin", {
                F("_1",            0x00, FIELD_PADDING, 4),
                F("_2",            0x04, FIELD_PADDING, 4),
                F("_3",            0x08, FIELD_PADDING, 248),
                F("dwLevelNo",     0x1D0, FIELD_DWORD, 4, "", 0, "area ID"),
                F("_4",            0x1D4, FIELD_PADDING, 88),
                F("pNextLevel",    0x22C, FIELD_POINTER, 4, "Level"),
            }};
            g_structs["Level"] = s;
        }

        // PlayerData
        {
            StructDef s = { "PlayerData", 0x170, "builtin", {
                F("szName",     0x00, FIELD_STRING, 16, "", 0, "character name"),
            }};
            g_structs["PlayerData"] = s;
        }

        // ItemData
        {
            StructDef s = { "ItemData", 0x90, "builtin", {
                F("dwQuality",      0x00, FIELD_DWORD, 4),
                F("_1",             0x04, FIELD_PADDING, 8),
                F("dwItemFlags",    0x0C, FIELD_DWORD, 4),
                F("_2",             0x10, FIELD_PADDING, 24),
                F("dwFileIndex",    0x28, FIELD_DWORD, 4),
                F("_3",             0x2C, FIELD_PADDING, 24),
                F("BodyLocation",   0x44, FIELD_BYTE, 1),
                F("ItemLocation",   0x45, FIELD_BYTE, 1),
                F("_4",             0x46, FIELD_PADDING, 35),
                F("NodePage",       0x69, FIELD_BYTE, 1, "", 0, "1=storage,2=belt,3=equipped"),
            }};
            g_structs["ItemData"] = s;
        }

        // Inventory
        {
            StructDef s = { "Inventory", 0x30, "builtin", {
                F("dwSignature",    0x00, FIELD_DWORD, 4),
                F("_1",             0x04, FIELD_PADDING, 4),
                F("pOwner",         0x08, FIELD_POINTER, 4, "UnitAny"),
                F("pFirstItem",     0x0C, FIELD_POINTER, 4, "UnitAny"),
                F("pLastItem",      0x10, FIELD_POINTER, 4, "UnitAny"),
                F("_2",             0x14, FIELD_PADDING, 12),
                F("pCursorItem",    0x20, FIELD_POINTER, 4, "UnitAny"),
                F("_3",             0x24, FIELD_PADDING, 4),
                F("dwItemCount",    0x28, FIELD_DWORD, 4),
            }};
            g_structs["Inventory"] = s;
        }

        // StatList
        {
            StructDef s = { "StatList", 0x40, "builtin", {
                F("_1",             0x00, FIELD_PADDING, 4),
                F("pUnit",          0x04, FIELD_POINTER, 4, "UnitAny"),
                F("dwUnitType",     0x08, FIELD_DWORD, 4),
                F("dwUnitId",       0x0C, FIELD_DWORD, 4),
                F("_2",             0x10, FIELD_PADDING, 20),
                F("pStat",          0x24, FIELD_POINTER, 4),
                F("wStatCount1",    0x28, FIELD_WORD, 2),
                F("wSize",          0x2A, FIELD_WORD, 2),
                F("pPrevLink",      0x2C, FIELD_POINTER, 4, "StatList"),
                F("_3",             0x30, FIELD_PADDING, 4),
                F("pPrev",          0x34, FIELD_POINTER, 4, "StatList"),
                F("_4",             0x38, FIELD_PADDING, 4),
                F("pNext",          0x3C, FIELD_POINTER, 4, "StatList"),
            }};
            g_structs["StatList"] = s;
        }

        // ItemPath (for items in inventory/belt)
        {
            StructDef s = { "ItemPath", 0x14, "builtin", {
                F("_1",         0x00, FIELD_PADDING, 12),
                F("dwPosX",     0x0C, FIELD_DWORD, 4, "", 0, "grid X / belt slot"),
                F("dwPosY",     0x10, FIELD_DWORD, 4, "", 0, "grid Y"),
            }};
            g_structs["ItemPath"] = s;
        }

        // MonsterData
        {
            StructDef s = { "MonsterData", 0x60, "builtin", {
                F("_1",         0x00, FIELD_PADDING, 22),
                F("fBoss",      0x16, FIELD_BYTE, 1, "", 0, "bit0=boss,bit1=champion,bit2=minion"),
            }};
            g_structs["MonsterData"] = s;
        }
    }
}

namespace StructRegistry {

    void Init() {
        std::lock_guard<std::mutex> lock(g_mutex);
        RegisterBuiltinStructs();

        // Try to load additional structs from game directory
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string dir(exePath);
        dir = dir.substr(0, dir.find_last_of("\\/") + 1);
        LoadFromFile(dir + "structs.json");
    }

    int LoadFromFile(const std::string& path) {
        int count = 0;
        try {
            std::ifstream f(path);
            if (!f.is_open()) return 0;

            rjson data = rjson::parse(f);
            if (!data.is_object()) return 0;

            for (auto it = data.begin(); it != data.end(); ++it) {
                std::string sname = it.key();
                rjson jStruct = it.value();
                StructDef def;
                def.name = sname;
                def.size = jStruct.value("size", 0);
                def.source = jStruct.value("source", "json");

                if (jStruct.contains("fields") && jStruct["fields"].is_array()) {
                    for (size_t fi = 0; fi < jStruct["fields"].size(); fi++) {
                        rjson jField = jStruct["fields"][fi];
                        FieldDef fd;
                        fd.name = jField.value("name", "");
                        fd.offset = jField.value("offset", 0);
                        fd.size = jField.value("size", 4);
                        fd.pointsTo = jField.value("points_to", "");
                        fd.arrayCount = jField.value("array_count", 0);
                        fd.comment = jField.value("comment", "");

                        std::string typeStr = jField.value("type", "dword");
                        if (typeStr == "byte") fd.type = FIELD_BYTE;
                        else if (typeStr == "word") fd.type = FIELD_WORD;
                        else if (typeStr == "dword") fd.type = FIELD_DWORD;
                        else if (typeStr == "int") fd.type = FIELD_INT;
                        else if (typeStr == "float") fd.type = FIELD_FLOAT;
                        else if (typeStr == "pointer") fd.type = FIELD_POINTER;
                        else if (typeStr == "string") fd.type = FIELD_STRING;
                        else if (typeStr == "wstring") fd.type = FIELD_WSTRING;
                        else if (typeStr == "array") fd.type = FIELD_ARRAY;
                        else fd.type = FIELD_PADDING;

                        def.fields.push_back(fd);
                    }
                }

                g_structs[sname] = def;
                count++;
            }
        } catch (...) {}
        return count;
    }

    bool SaveToFile(const std::string& path) {
        std::lock_guard<std::mutex> lock(g_mutex);
        try {
            rjson data;
            const char* typeNames[] = {"byte","word","dword","int","float","pointer","string","wstring","array","padding"};

            for (auto it = g_structs.begin(); it != g_structs.end(); ++it) {
                const std::string& name = it->first;
                const StructDef& def = it->second;
                rjson jStruct;
                jStruct["size"] = def.size;
                jStruct["source"] = def.source;

                rjson fields = rjson::array();
                for (auto& f : def.fields) {
                    rjson jf;
                    jf["name"] = f.name;
                    jf["offset"] = f.offset;
                    jf["type"] = typeNames[f.type];
                    jf["size"] = f.size;
                    if (!f.pointsTo.empty()) jf["points_to"] = f.pointsTo;
                    if (f.arrayCount > 0) jf["array_count"] = f.arrayCount;
                    if (!f.comment.empty()) jf["comment"] = f.comment;
                    fields.push_back(jf);
                }
                jStruct["fields"] = fields;
                data[name] = jStruct;
            }

            std::ofstream fout(path);
            fout << data.dump(2) << std::endl;
            return true;
        } catch (...) {
            return false;
        }
    }

    const StructDef* GetStruct(const std::string& name) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_structs.find(name);
        return it != g_structs.end() ? &it->second : nullptr;
    }

    void AddStruct(const StructDef& def) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_structs[def.name] = def;
    }

    std::vector<std::string> ListStructs() {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<std::string> names;
        for (auto& kv : g_structs) names.push_back(kv.first);
        return names;
    }

    std::map<std::string, StructDef> GetAllStructs() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_structs;
    }

    int GetStructCount() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return (int)g_structs.size();
    }
}
