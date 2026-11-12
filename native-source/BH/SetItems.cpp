#define NOMINMAX
#include "SetItems.h"
#include "D2Ptrs.h"
#include "D2Helpers.h"
#include "Constants.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace {
using json = nlohmann::json;
static_assert(sizeof(SetItemsTxt) == 0x1B8, "S13 set piece layout changed");
static_assert(sizeof(SetsTxt) == 0x128, "S13 set group layout changed");

bool Readable(const void* address, size_t bytes) {
    if (!address || !bytes) return false;
    MEMORY_BASIC_INFORMATION m = {};
    if (!VirtualQuery(address, &m, sizeof(m)) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(m.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    const auto start = reinterpret_cast<uintptr_t>(address);
    const auto end = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
    return end >= start && bytes <= end - start;
}
json Failure(const char* error) { return {{"ok", false}, {"error", error}}; }
sgptDataTable* Tables() {
    if (!Readable(p_D2COMMON_sgptDataTable, sizeof(sgptDataTable*))) return nullptr;
    sgptDataTable* tables = *p_D2COMMON_sgptDataTable;
    return Readable(tables, sizeof(*tables)) ? tables : nullptr;
}
bool SetTables(sgptDataTable* t) {
    return t && t->dwSetItemsRecs && t->dwSetItemsRecs <= 32768 &&
        t->dwSetsRecs && t->dwSetsRecs <= 32768 &&
        Readable(t->pSetItemsTxt, size_t(t->dwSetItemsRecs) * sizeof(SetItemsTxt)) &&
        Readable(t->pSetsTxt, size_t(t->dwSetsRecs) * sizeof(SetsTxt));
}
std::string Text(const char* data, size_t capacity) {
    return std::string(data, strnlen(data, capacity));
}
std::string Locale(WORD id) {
    const wchar_t* text = D2LANG_GetLocaleText(id);
    if (!Readable(text, sizeof(wchar_t))) return {};
    size_t count = 0;
    while (count < 255 && Readable(text + count, sizeof(wchar_t)) && text[count]) ++count;
    if (count == 255 || !Readable(text + count, sizeof(wchar_t))) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(count), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || bytes > 1024) return {};
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(count), &result[0], bytes, nullptr, nullptr);
    return result;
}
json Bonus(const ItemsTxtStat* stats, unsigned length) {
    json result = json::array();
    for (unsigned i = 0; i < length; ++i) {
        const auto& stat = stats[i];
        // Compiled properties use -1 for an empty slot; property zero is valid.
        if (static_cast<int32_t>(stat.dwProp) >= 0)
            result.push_back({{"property_id", stat.dwProp}, {"parameter", stat.dwPar},
                              {"min", stat.dwMin}, {"max", stat.dwMax}});
    }
    return result;
}
json Piece(const sgptDataTable* tables, unsigned id) {
    const SetItemsTxt& item = tables->pSetItemsTxt[id];
    if (item.nSetId >= tables->dwSetsRecs) return Failure("set piece refers to an invalid group");
    const SetsTxt& set = tables->pSetsTxt[item.nSetId];
    if (set.nSetItems < 1 || set.nSetItems > 6) return Failure("set group has invalid piece count");
    json partial = json::array();
    const ItemsTxtStat* thresholds[] = {set.pBoni2, set.pBoni3, set.pBoni4, set.pBoni5};
    for (unsigned n = 0; n < 4; ++n) {
        json bonuses = Bonus(thresholds[n], 2);
        if (!bonuses.empty()) partial.push_back({{"equipped_pieces", n + 2}, {"properties", std::move(bonuses)}});
    }
    return {{"id", id}, {"table_index", item.dwTblIndex},
            {"name", Text(item.szName, sizeof(item.szName))}, {"set_id", item.nSetId},
            {"set_name", Locale(set.wStringId)}, {"set_name_string_id", set.wStringId},
            {"code", GetItemCode(item.szCode)}, {"level", item.wLvl},
            {"required_level", item.wLvlReq}, {"piece_count", set.nSetItems},
            {"rarity", item.dwRarity}, {"add_function", item.nAddFunc},
            {"properties", Bonus(item.hStats, 9)},
            {"piece_partial_bonuses", Bonus(item.hGreenStats, 10)},
            {"partial_bonuses", std::move(partial)}, {"full_bonuses", Bonus(set.pFBoni, 8)}};
}
}

namespace SetItems {
json Catalog(const json& args) {
    if (!args.is_object()) return Failure("invalid catalog arguments");
    unsigned limit = 64, offset = 0;
    if (args.contains("limit")) {
        const auto& v = args.at("limit");
        if (!v.is_number_integer() || (v.is_number_unsigned() ? v.get<uint64_t>() == 0 || v.get<uint64_t>() > 128 :
            v.get<int64_t>() <= 0 || v.get<int64_t>() > 128)) return Failure("invalid catalog limit");
        limit = v.get<unsigned>();
    }
    if (args.contains("offset")) {
        const auto& v = args.at("offset");
        if (!v.is_number_unsigned() || v.get<uint64_t>() > 32768) return Failure("invalid catalog offset");
        offset = v.get<unsigned>();
    }
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!Readable(player, sizeof(*player)) || player->dwType != UNIT_PLAYER) return Failure("player unavailable");
    sgptDataTable* t = Tables();
    if (!SetTables(t)) return Failure("set tables unavailable");
    const unsigned end = std::min<unsigned>(t->dwSetItemsRecs, offset + limit);
    json rows = json::array();
    for (unsigned i = offset; i < end; ++i) {
        json row = Piece(t, i);
        if (!row.value("ok", true)) return row;
        rows.push_back(std::move(row));
    }
    return {{"ok", true}, {"table", "sets"}, {"offset", offset},
            {"total", t->dwSetItemsRecs}, {"records", std::move(rows)},
            {"truncated", offset + limit < t->dwSetItemsRecs}};
}
json Describe(UnitAny* item) {
    if (!Readable(item, sizeof(*item)) || item->dwType != UNIT_ITEM ||
        !Readable(item->pItemData, sizeof(*item->pItemData))) return Failure("invalid item unit");
    if (item->pItemData->dwQuality != 5) return json::object();
    sgptDataTable* t = Tables();
    if (!SetTables(t)) return Failure("set tables unavailable");
    const DWORD id = item->pItemData->dwFileIndex;
    if (id >= t->dwSetItemsRecs) return Failure("set file index outside loaded table");
    json result = Piece(t, id);
    if (!result.value("ok", true)) return result;
    const SetItemsTxt& record = t->pSetItemsTxt[id];
    const ItemsTxt* base = D2COMMON_GetItemText(item->dwTxtFileNo);
    if (!Readable(base, sizeof(*base))) return Failure("set item base text unavailable");
    const std::string baseCode = GetItemCode(base->szCode);
    const std::string setCode = GetItemCode(record.szCode);
    if (baseCode != setCode) {
        json failure = Failure("set item base code does not match selected piece");
        failure["item_id"] = item->dwUnitId;
        failure["file_index"] = id;
        failure["class_id"] = item->dwTxtFileNo;
        failure["base_code"] = baseCode;
        failure["set_code"] = setCode;
        return failure;
    }
    result["set_base_required_strength"] = base->wreqstr;
    result["set_base_required_dexterity"] = base->wreqdex;
    result["set_item_id"] = id;
    result["set_item_name"] = result["name"];
    result["set_table_index"] = result["table_index"];
    result.erase("table_index");
    result["set_definition_properties"] = std::move(result["properties"]);
    result["set_table_required_level"] = result["required_level"];
    result["set_table_level"] = result["level"];
    result["set_rarity"] = result["rarity"];
    result["set_add_function"] = result["add_function"];
    result.erase("id");
    result.erase("name");
    result.erase("properties");
    result.erase("required_level");
    result.erase("level");
    result.erase("rarity");
    result.erase("add_function");
    result.erase("code");
    // This getter is already bound and consumed by the native item UI. It computes
    // the effective level requirement, including item properties and player context.
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (Readable(player, sizeof(*player)) && player->dwType == UNIT_PLAYER)
        result["set_effective_required_level"] = D2COMMON_11015_GetItemLevelRequirement(item, player);
    else result["set_effective_required_level"] = nullptr;
    const int requiredClass = D2COMMON_10822_GetItemRequiredClass(item);
    result["set_required_class_id"] = requiredClass >= 0 && requiredClass < 7 ? json(requiredClass) : json(nullptr);
    return result;
}
}
