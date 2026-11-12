#pragma once

struct WssCorruptionProperty {
    D2PropertyStrc native{};
    WORD id = 0;
    WORD parameter = 0;
    WORD chance = 0;
    int minimum = 0;
    int maximum = 0;
    bool marker = false;
    bool corruptor = false;
};
struct WssCorruptionOutput {
    std::array<WssCorruptionProperty, 5> properties{};
    unsigned propertyCount = 0;
    unsigned markerCount = 0;
    unsigned corruptorCount = 0;
    unsigned effectCount = 0;
    int marker = 0;
    int corruptor = 0;
    bool allChancesCertain = true;
};
struct WssCorruptionSelection {
    unsigned randomizerIndex = 0;
    unsigned recipeIndex = 0;
    int rollMinimum = 0;
    int rollMaximum = 0;
    int roll = 0;
    int threshold = 0;
    std::array<CubeInputItem, 2> selectors{};
    WssCorruptionOutput output{};
};
bool WssPropertyHasStat(const sgptDataTable* records, DWORD propertyId,
    WORD statId) {
    if (!records || propertyId >= records->dwProportiesRecs ||
        !Readable(records->pPropertiesTxt + propertyId,
            sizeof(PropertiesTxt)))
        return false;
    const PropertiesTxt& property = records->pPropertiesTxt[propertyId];
    for (const WORD stat : property.wStat)
        if (stat == statId) return true;
    return false;
}
bool WssInputMatchesItem(const CubeInputItem& selector, UnitAny* item,
    const D2ItemDataTbl* bases, const sgptDataTable* records) {
    if (!item || !Readable(item, sizeof(UnitAny)) ||
        item->dwType != UNIT_ITEM ||
        !Readable(item->pItemData, sizeof(ItemData)) ||
        !bases || !bases->pItemsTxt || !records ||
        item->dwTxtFileNo >= static_cast<DWORD>(bases->nItemsTxtRecordCount))
        return false;
    WORD flags = 0;
    memcpy(&flags, &selector, sizeof(flags));
    const ItemsTxt& base = bases->pItemsTxt[item->dwTxtFileNo];
    bool codeMatches = !(flags & 0x01);
    bool upgradedCodeMatches = !(flags & 0x80);
    if (flags & 0x01) {
        if (selector.Item == 0xffff) {
            codeMatches = true;
            upgradedCodeMatches = true;
        } else if (selector.Item <
                static_cast<WORD>(bases->nItemsTxtRecordCount)) {
            const ItemsTxt& selectedBase = bases->pItemsTxt[selector.Item];
            const DWORD itemCode = base.dwcode;
            upgradedCodeMatches =
                itemCode == selectedBase.dwcode ||
                itemCode == selectedBase.dwnormcode ||
                itemCode == selectedBase.dwubercode ||
                itemCode == selectedBase.dwultracode;
            codeMatches = (flags & 0x80) ? upgradedCodeMatches :
                item->dwTxtFileNo == selector.Item;
        } else {
            return false;
        }
    }
    bool typeMatches = !(flags & 0x02);
    if (flags & 0x02) {
        if (selector.Item >= records->dwItemsTypeRecs ||
            !records->pItemsTypeTxt)
            return false;
        typeMatches = D2COMMON_IsMatchingType(
            item, selector.Item) != FALSE;
    }
    const bool quantityMatches = !selector.Quantity ||
        D2COMMON_GetUnitStat(item, STAT_AMMOQUANTITY, 0) >= selector.Quantity;
    const bool uniqueIdMatches = item->pItemData->dwFileIndex < 0xffff &&
        item->pItemData->dwFileIndex + 1 == selector.ItemID;
    const int sockets = D2COMMON_GetUnitStat(item, STAT_SOCKETS, 0);
    const RoguelikeLoot::WssInputFacts facts{
        flags, selector.Quality,
        static_cast<std::uint8_t>(item->pItemData->dwQuality),
        codeMatches, typeMatches, uniqueIdMatches, quantityMatches,
        sockets > 0, (item->pItemData->dwFlags & ITEM_ETHEREAL) != 0,
        (item->pItemData->dwFlags & ITEM_RUNEWORD) != 0,
        base.dwcode == base.dwnormcode,
        base.dwcode == base.dwubercode,
        base.dwcode == base.dwultracode,
        upgradedCodeMatches};
    return RoguelikeLoot::WssInputMatches(facts);
}
bool WssInputMatchesIngredient(const CubeInputItem& selector,
    int wssBaseId, const D2ItemDataTbl* bases, const sgptDataTable* records) {
    if (!bases || wssBaseId < 0 ||
        wssBaseId >= bases->nItemsTxtRecordCount ||
        !records || !records->pItemsTypeTxt)
        return false;
    WORD flags = 0;
    memcpy(&flags, &selector, sizeof(flags));
    const ItemsTxt& base = bases->pItemsTxt[wssBaseId];
    std::array<WORD, 64> types{};
    unsigned typeCount = 0;
    if (!ItemTypeAncestry(records, base.nType, types, typeCount))
        return false;
    bool typeMatches = !(flags & 0x02);
    if (flags & 0x02) {
        typeMatches = false;
        for (unsigned index = 0; index < typeCount; ++index)
            typeMatches = typeMatches || types[index] == selector.Item;
    }
    const bool codeMatches = !(flags & 0x01) ||
        selector.Item == wssBaseId;
    const bool upgradedCodeMatches = !(flags & 0x80) ||
        selector.Item == wssBaseId;
    const bool quantityMatches = !selector.Quantity ||
        selector.Quantity <= 1;
    const RoguelikeLoot::WssInputFacts facts{
        flags, selector.Quality, 0,
        codeMatches, typeMatches, false, quantityMatches,
        false, false, false,
        base.dwcode == base.dwnormcode,
        base.dwcode == base.dwubercode,
        base.dwcode == base.dwultracode,
        upgradedCodeMatches};
    return RoguelikeLoot::WssInputMatches(facts);
}
bool WssRecipeInputsMatch(const CubeMainTxt& recipe, UnitAny* item,
    int wssBaseId, const D2ItemDataTbl* bases,
    const sgptDataTable* records) {
    if (recipe.bNuminputs != 2) return false;
    return WssInputMatchesItem(
            recipe.InputItem[0], item, bases, records) &&
        WssInputMatchesIngredient(
            recipe.InputItem[1], wssBaseId, bases, records);
}
bool WssReadOutput(const CubeMainTxt& recipe,
    const sgptDataTable* records, WssCorruptionOutput& output) {
    if (!records || !records->pPropertiesTxt ||
        recipe.OutputItem[0].Type !=
            RoguelikeLoot::kWssUseItemOutputType ||
        recipe.OutputItem[0].nQuality != 0)
        return false;
    output = WssCorruptionOutput{};
    for (const auto& mod : recipe.OutputItem[0].sMods) {
        const bool hasFields = mod.bModChance || mod.wModParam ||
            mod.wModMin || mod.wModMax;
        if (mod.dwMod == static_cast<DWORD>(-1)) {
            if (hasFields) return false;
            continue;
        }
        if (!mod.dwMod && !hasFields) continue;
        if (output.propertyCount == output.properties.size() ||
            mod.dwMod >= records->dwProportiesRecs)
            return false;
        const int minimum = static_cast<std::int16_t>(mod.wModMin);
        const int maximum = static_cast<std::int16_t>(mod.wModMax);
        if (minimum > maximum) return false;
        const bool marker = WssPropertyHasStat(
            records, mod.dwMod, STAT_CORRUPTED);
        const bool corruptor = WssPropertyHasStat(
            records, mod.dwMod, STAT_CORRUPTOR);
        if (marker && corruptor) return false;
        WssCorruptionProperty& property =
            output.properties[output.propertyCount++];
        property.id = static_cast<WORD>(mod.dwMod);
        property.parameter = mod.wModParam;
        property.chance = mod.bModChance;
        property.minimum = minimum;
        property.maximum = maximum;
        property.marker = marker;
        property.corruptor = corruptor;
        property.native.nProperty = mod.dwMod;
        property.native.nLayer = mod.wModParam;
        property.native.nMin = static_cast<DWORD>(minimum);
        property.native.nMax = static_cast<DWORD>(maximum);
        if (marker) {
            ++output.markerCount;
            if (minimum != maximum) return false;
            output.marker = minimum;
        } else if (corruptor) {
            ++output.corruptorCount;
            output.corruptor = minimum == maximum ? minimum : -1;
        } else {
            ++output.effectCount;
        }
        output.allChancesCertain = output.allChancesCertain &&
            RoguelikeLoot::IsCertainWssPropertyChance(mod.bModChance);
    }
    return true;
}
bool WssOutputIsCorruption(const CubeMainTxt& recipe,
    const sgptDataTable* records, WssCorruptionOutput& output) {
    if (!WssReadOutput(recipe, records, output) ||
        recipe.OutputItem[1].Type || recipe.OutputItem[2].Type)
        return false;
    return output.markerCount == 1 && output.corruptorCount == 1 &&
        RoguelikeLoot::IsValidWssCorruptionOutput(
            recipe.OutputItem[0].Type,
            recipe.OutputItem[0].nQuality,
            output.marker, output.corruptor,
            static_cast<int>(output.effectCount),
            output.allChancesCertain);
}
bool WssCorruptnumRange(const CubeMainTxt& recipe,
    const sgptDataTable* records, int wssBaseId,
    int& minimum, int& maximum) {
    WssCorruptionOutput output;
    if (!WssReadOutput(recipe, records, output) ||
        output.propertyCount != 1)
        return false;
    const WssCorruptionProperty& property = output.properties[0];
    if (!property.corruptor ||
        !RoguelikeLoot::IsValidWssCorruptnumOutput(
            recipe.OutputItem[0].Type,
            recipe.OutputItem[0].nQuality,
            output.propertyCount, output.markerCount,
            output.corruptorCount, output.effectCount,
            output.corruptor, property.minimum, property.maximum,
            output.allChancesCertain) ||
        !RoguelikeLoot::IsValidWssRandomizerItemReturn(
            recipe.OutputItem[1].Type, recipe.OutputItem[1].Item,
            recipe.OutputItem[1].nQuality, wssBaseId,
            recipe.OutputItem[1].nQuantity, recipe.OutputItem[2].Type))
        return false;
    minimum = property.minimum;
    maximum = property.maximum;
    return true;
}
using WssCubeMainCount = int (__stdcall*)();
using WssCubeMainRecord = CubeMainTxt* (__stdcall*)(int);
using WssApplyProperty = void (__stdcall*)(UnitAny*, D2PropertyStrc*, DWORD);
struct WssBindings {
    WssCubeMainCount count = nullptr;
    WssCubeMainRecord record = nullptr;
    WssApplyProperty apply = nullptr;
};
WssBindings wssBindings;
static_assert(sizeof(CubeMainTxt) == 0x148, "pinned S13 cube recipe stride");
struct WssCubeMainTable {
    WssCubeMainCount count = nullptr;
    WssCubeMainRecord record = nullptr;
    DWORD total = 0;
};
bool PrepareWssBindings(std::string& error) {
    if (wssBindings.count && wssBindings.record && wssBindings.apply) return true;
    if (!LabProfile::VerifyCommon(error)) return false;
    HMODULE module = GetModuleHandleA("D2Common.dll");
    if (!module) { error = "S13 D2Common module unavailable"; return false; }
    const FARPROC countExport = GetProcAddress(module, MAKEINTRESOURCEA(10675));
    const FARPROC recordExport = GetProcAddress(module, MAKEINTRESOURCEA(10783));
    const BYTE* countCode = reinterpret_cast<const BYTE*>(countExport);
    const BYTE* recordCode = reinterpret_cast<const BYTE*>(recordExport);
    const BYTE* property = reinterpret_cast<const BYTE*>(module) + 0x47730;
    const BYTE propertySignature[] = {0x56,0x8b,0x74,0x24,0x08,0x85,0xf6};
    if (!Readable(countCode, 6) || !Readable(recordCode, 2) ||
        countCode[0] != 0xA1 || countCode[5] != 0xC3 ||
        recordCode[0] != 0x8B || recordCode[1] != 0x0D ||
        !Readable(property, sizeof(propertySignature)) ||
        memcmp(property, propertySignature, sizeof(propertySignature)) ||
        reinterpret_cast<const BYTE*>(GetProcAddress(module, MAKEINTRESOURCEA(10914))) != property) {
        error = "unrecognized S13 CubeMain bindings";
        return false;
    }
    wssBindings = {reinterpret_cast<WssCubeMainCount>(countExport),
        reinterpret_cast<WssCubeMainRecord>(recordExport),
        reinterpret_cast<WssApplyProperty>(const_cast<BYTE*>(property))};
    return true;
}
bool WssLoadCubeMainTable(WssCubeMainTable& table, std::string& error) {
    if (!PrepareWssBindings(error)) return false;
    const auto count = wssBindings.count;
    const auto record = wssBindings.record;
    const int total = count();
    if (total <= 0 || total > 32768) {
        error = "S13 D2Common CubeMain record count is invalid";
        return false;
    }
    table = {count, record, static_cast<DWORD>(total)};
    return true;
}
bool WssCubeMainRow(const WssCubeMainTable& table, DWORD index,
    const CubeMainTxt*& row, std::string& error) {
    if (!table.record || index >= table.total) {
        error = "S13 CubeMain row index is outside the loaded table";
        return false;
    }
    CubeMainTxt* loaded = table.record(static_cast<int>(index));
    if (!Readable(loaded, sizeof(*loaded))) {
        error = "S13 CubeMain record is unreadable";
        return false;
    }
    row = loaded;
    return true;
}
bool WssPropertiesTableValid(const sgptDataTable* records) {
    return records && Readable(records, sizeof(*records)) &&
        records->pPropertiesTxt && records->dwProportiesRecs &&
        records->dwProportiesRecs <= 32768 &&
        Readable(records->pPropertiesTxt,
            size_t(records->dwProportiesRecs) * sizeof(PropertiesTxt));
}
int WssBaseId(const D2ItemDataTbl* bases) {
    if (!bases || !bases->pItemsTxt ||
        bases->nItemsTxtRecordCount <= 0 ||
        bases->nItemsTxtRecordCount > 32768 ||
        !Readable(bases->pItemsTxt,
            size_t(bases->nItemsTxtRecordCount) * sizeof(ItemsTxt)))
        return -1;
    int result = -1;
    for (int index = 0; index < bases->nItemsTxtRecordCount; ++index) {
        if (!RoguelikeLoot::IsWssBaseCode(
                bases->pItemsTxt[index].szCode))
            continue;
        if (result >= 0) return -1;
        result = index;
    }
    return result;
}
bool WssFindRandomizer(const WssCubeMainTable& cube,
    const sgptDataTable* records, const D2ItemDataTbl* bases,
    UnitAny* item, int wssBaseId, unsigned& rowIndex,
    int& minimum, int& maximum, std::string& error) {
    for (DWORD index = 0; index < cube.total; ++index) {
        const CubeMainTxt* row = nullptr;
        if (!WssCubeMainRow(cube, index, row, error)) return false;
        const CubeMainTxt& recipe = *row;
        if (!recipe.bEnabled || recipe.bOp != 18 ||
            recipe.dwParam != STAT_CORRUPTOR || recipe.dwValue != 0 ||
            !WssRecipeInputsMatch(
                recipe, item, wssBaseId, bases, records))
            continue;
        if (!WssCorruptnumRange(
                recipe, records, wssBaseId, minimum, maximum)) {
            error = "matching S13 WSS randomizer has an unsupported output";
            return false;
        }
        rowIndex = index;
        return true;
    }
    error = "loaded S13 CubeMain has no matching WSS corruptnum randomizer";
    return false;
}
bool WssSelectCorruption(UnitAny* item, DWORD seed,
    WssCorruptionSelection& selected, std::string& error) {
    D2ItemDataTbl* bases = nullptr;
    sgptDataTable* records = nullptr;
    if (!RewardTables(bases, records)) {
        error = "loaded S13 reward base or item-type tables are unavailable";
        return false;
    }
    if (!WssPropertiesTableValid(records)) {
        error = "loaded S13 Properties.txt table is unavailable";
        return false;
    }
    WssCubeMainTable cube;
    if (!WssLoadCubeMainTable(cube, error)) return false;
    const int wssBaseId = WssBaseId(bases);
    if (wssBaseId < 0) {
        error = "loaded S13 item table has no unique WSS base code";
        return false;
    }
    int rollMinimum = 0, rollMaximum = 0;
    unsigned randomizerIndex = 0;
    if (!WssFindRandomizer(cube, records, bases, item, wssBaseId,
            randomizerIndex, rollMinimum, rollMaximum, error))
        return false;
    unsigned totalWeight = 0;
    int previous = rollMinimum - 1;
    for (DWORD index = 0; index < cube.total; ++index) {
        const CubeMainTxt* row = nullptr;
        if (!WssCubeMainRow(cube, index, row, error)) return false;
        const CubeMainTxt& recipe = *row;
        if (!recipe.bEnabled || recipe.bOp != 16 ||
            recipe.dwParam != STAT_CORRUPTOR ||
            !WssRecipeInputsMatch(
                recipe, item, wssBaseId, bases, records))
            continue;
        const int threshold = recipe.dwValue > INT_MAX ?
            INT_MAX : static_cast<int>(recipe.dwValue);
        const unsigned weight = RoguelikeLoot::WssThresholdWeight(
            previous, threshold, rollMinimum, rollMaximum);
        WssCorruptionOutput output;
        if (weight && WssOutputIsCorruption(recipe, records, output))
            totalWeight += weight;
        previous = RoguelikeLoot::WssThresholdCoverage(
            previous, threshold);
    }
    if (!totalWeight) {
        error = "loaded S13 WSS selector has no non-bricking unique outcomes";
        return false;
    }
    std::minstd_rand random(seed ? seed : 1);
    std::uniform_int_distribution<unsigned> ticket(0, totalWeight - 1);
    unsigned remaining = ticket(random);
    previous = rollMinimum - 1;
    for (DWORD index = 0; index < cube.total; ++index) {
        const CubeMainTxt* row = nullptr;
        if (!WssCubeMainRow(cube, index, row, error)) return false;
        const CubeMainTxt& recipe = *row;
        if (!recipe.bEnabled || recipe.bOp != 16 ||
            recipe.dwParam != STAT_CORRUPTOR ||
            !WssRecipeInputsMatch(
                recipe, item, wssBaseId, bases, records))
            continue;
        const int threshold = recipe.dwValue > INT_MAX ?
            INT_MAX : static_cast<int>(recipe.dwValue);
        const unsigned weight = RoguelikeLoot::WssThresholdWeight(
            previous, threshold, rollMinimum, rollMaximum);
        WssCorruptionOutput output;
        if (weight && WssOutputIsCorruption(recipe, records, output)) {
            if (remaining < weight) {
                selected.randomizerIndex = randomizerIndex;
                selected.recipeIndex = index;
                selected.rollMinimum = rollMinimum;
                selected.rollMaximum = rollMaximum;
                const int lower = previous + 1 > rollMinimum ?
                    previous + 1 : rollMinimum;
                selected.roll = lower + static_cast<int>(remaining);
                selected.threshold = threshold;
                selected.selectors = {
                    recipe.InputItem[0], recipe.InputItem[1]};
                selected.output = output;
                error.clear();
                return true;
            }
            remaining -= weight;
        }
        previous = RoguelikeLoot::WssThresholdCoverage(
            previous, threshold);
    }
    error = "loaded S13 WSS valid-outcome weights changed during selection";
    return false;
}
json WssCorruptionJson(const WssCorruptionSelection& selected) {
    json properties = json::array();
    for (unsigned index = 0;
         index < selected.output.propertyCount; ++index) {
        const WssCorruptionProperty& property =
            selected.output.properties[index];
        properties.push_back({
            {"property_id", property.id},
            {"parameter", property.parameter},
            {"minimum", property.minimum},
            {"maximum", property.maximum},
            {"chance", property.chance},
            {"marker", property.marker},
            {"corruptor", property.corruptor}});
    }
    json inputs = json::array();
    for (const CubeInputItem& selector : selected.selectors) {
        WORD flags = 0;
        memcpy(&flags, &selector, sizeof(flags));
        inputs.push_back({
            {"flags", flags}, {"item", selector.Item},
            {"item_id", selector.ItemID},
            {"quality", selector.Quality},
            {"quantity", selector.Quantity}});
    }
    return {
        {"source", "loaded_s13_cubemain_wss"},
        {"randomizer_row", selected.randomizerIndex},
        {"recipe_row", selected.recipeIndex},
        {"roll_minimum", selected.rollMinimum},
        {"roll_maximum", selected.rollMaximum},
        {"roll", selected.roll}, {"threshold", selected.threshold},
        {"marker", selected.output.marker},
        {"corruptor", selected.output.corruptor},
        {"inputs", inputs}, {"properties", properties}};
}
bool ApplyWssCorruption(void* game, UnitAny* item, DWORD seed,
    WssCorruptionSelection& selected, std::string& error) {
    if (!game || !item) {
        error = "WSS corruption needs a live generated item and game";
        return false;
    }
    if (!WssSelectCorruption(item, seed, selected, error)) return false;
    const DWORD propertyPool = *reinterpret_cast<const DWORD*>(
        static_cast<const BYTE*>(game) + 0x70);
    if (!propertyPool) {
        error = "S13 native item property pool is unavailable";
        return false;
    }
    const auto apply = wssBindings.apply;
    for (unsigned index = 0;
         index < selected.output.propertyCount; ++index)
        apply(item, &selected.output.properties[index].native, propertyPool);
    error.clear();
    return true;
}
