import datetime
import sys
from ctypes import *
from xml.etree import ElementTree

UCPTRIE_TYPE_FAST = 0
UCPTRIE_TYPE_SMALL = 1

UCPTRIE_VALUE_BITS_16 = 0
UCPTRIE_VALUE_BITS_32 = 1
UCPTRIE_VALUE_BITS_8 = 2

U_MAX_VERSION_LENGTH = 4
U_MAX_VERSION_STRING_LENGTH = 20

icu = cdll.icu

# U_CAPI const char * U_EXPORT2
# u_errorName(UErrorCode code);
icu.u_errorName.restype = c_char_p
icu.u_errorName.argtypes = [c_int]


def check_error(error: c_int):
    if error.value > 0:
        name = icu.u_errorName(error)
        raise RuntimeError(f"failed with {name.decode()} ({error.value})")


# U_CAPI void U_EXPORT2
# u_getVersion(UVersionInfo versionArray);
icu.u_getVersion.restype = None
icu.u_getVersion.argtypes = [c_void_p]
# U_CAPI void U_EXPORT2
# u_versionToString(const UVersionInfo versionArray, char *versionString);
icu.u_versionToString.restype = None
icu.u_versionToString.argtypes = [c_void_p, c_char_p]


def u_getVersion():
    info = (c_uint8 * U_MAX_VERSION_LENGTH)()
    icu.u_getVersion(info)
    str = (c_char * U_MAX_VERSION_STRING_LENGTH)()
    icu.u_versionToString(info, str)
    return str.value.decode()


# U_CAPI UMutableCPTrie * U_EXPORT2
# umutablecptrie_open(uint32_t initialValue, uint32_t errorValue, UErrorCode *pErrorCode);
icu.umutablecptrie_open.restype = c_void_p
icu.umutablecptrie_open.argtypes = [c_uint32, c_uint32, c_void_p]


def umutablecptrie_open(initial_value: int) -> c_void_p:
    error = c_int()
    trie = icu.umutablecptrie_open(initial_value, 0, byref(error))
    check_error(error)
    return trie


# U_CAPI void U_EXPORT2
# umutablecptrie_set(UMutableCPTrie *trie, UChar32 c, uint32_t value, UErrorCode *pErrorCode);
icu.umutablecptrie_set.restype = None
icu.umutablecptrie_set.argtypes = [c_void_p, c_uint32, c_uint32, c_void_p]


def umutablecptrie_set(mutable_trie: c_void_p, c: int, value: int):
    error = c_int()
    icu.umutablecptrie_set(mutable_trie, c, value, byref(error))
    check_error(error)


# U_CAPI void U_EXPORT2
# umutablecptrie_setRange(UMutableCPTrie *trie, UChar32 start, UChar32 end, uint32_t value, UErrorCode *pErrorCode);
icu.umutablecptrie_setRange.restype = None
icu.umutablecptrie_setRange.argtypes = [c_void_p, c_uint32, c_uint32, c_uint32, c_void_p]


def umutablecptrie_setRange(mutable_trie: c_void_p, start: int, end: int, value: int):
    error = c_int()
    icu.umutablecptrie_setRange(mutable_trie, start, end, value, byref(error))
    check_error(error)


# U_CAPI UCPTrie * U_EXPORT2
# umutablecptrie_buildImmutable(UMutableCPTrie *trie, UCPTrieType type, UCPTrieValueWidth valueWidth, UErrorCode *pErrorCode);
icu.umutablecptrie_buildImmutable.restype = c_void_p
icu.umutablecptrie_buildImmutable.argtypes = [c_void_p, c_int, c_int, c_void_p]


def umutablecptrie_buildImmutable(mutable_trie: c_void_p, typ: int, value_width: int) -> c_void_p:
    error = c_int()
    trie = icu.umutablecptrie_buildImmutable(mutable_trie, typ, value_width, byref(error))
    check_error(error)
    return trie


# U_CAPI int32_t U_EXPORT2
# ucptrie_toBinary(const UCPTrie *trie, void *data, int32_t capacity, UErrorCode *pErrorCode);
icu.ucptrie_toBinary.restype = c_int32
icu.ucptrie_toBinary.argtypes = [c_void_p, c_void_p, c_int32, c_void_p]


def ucptrie_toBinary(trie: c_void_p) -> Array[c_ubyte]:
    error = c_int()
    expected_size = icu.ucptrie_toBinary(trie, c_void_p(), 0, byref(error))

    data = (c_ubyte * expected_size)()
    error = c_int()
    acutal_size = icu.ucptrie_toBinary(trie, data, expected_size, byref(error))
    check_error(error)

    if acutal_size != expected_size:
        raise RuntimeError("apparently ucptrie_toBinary(nullptr, 0) only returns an estimate -> fix me")

    return data


def main():
    if len(sys.argv) != 3 or not sys.argv[1].endswith("ucd.nounihan.grouped.xml"):
        print("main.py <path to ucd.nounihan.grouped.xml> <path to unicode_width_overrides.xml>")
        exit(1)

    ns = {"ns": "http://www.unicode.org/ns/2003/ucd/1.0"}
    files = [ElementTree.parse(path).getroot() for path in sys.argv[1:]]
    time = datetime.datetime.utcnow().isoformat(timespec='seconds')
    unicode_version = files[0].find("./ns:description", ns).text
    icu_version = u_getVersion()
    mutable_trie = umutablecptrie_open(1)

    for root in files:
        for group in root.findall("./ns:repertoire/ns:group", ns):
            group_ea = group.get("ea")
            group_emoji = group.get("Emoji")
            group_epres = group.get("EPres")

            for char in group:
                ea = char.get("ea") or group_ea  # east-asian (width)
                emoji = char.get("Emoji") or group_emoji  # emoji
                epres = char.get("EPres") or group_epres  # emoji presentation
                if emoji == "Y" and epres == "Y":
                    value = 2
                else:
                    match ea:
                        case "A":  # ambiguous
                            value = 0
                        case "N" | "Na" | "H":  # neutral, narrow, half-width
                            value = 1
                        case "F" | "W":  # full-width, wide
                            value = 2
                        case _:
                            raise RuntimeError(f"unrecognized ea: {ea}")

                cp = char.get("cp")  # codepoint
                if cp is not None:
                    cp = int(cp, 16)
                    umutablecptrie_set(mutable_trie, cp, value)
                else:
                    cp_first = int(char.get("first-cp"), 16)
                    cp_last = int(char.get("last-cp"), 16)
                    umutablecptrie_setRange(mutable_trie, cp_first, cp_last, value)

    trie = umutablecptrie_buildImmutable(mutable_trie, UCPTRIE_TYPE_FAST, UCPTRIE_VALUE_BITS_8)
    data = ucptrie_toBinary(trie)

    print("// Generated by tools/CodepointWidthDetector/main.py")
    print(f"// on {time}Z from {unicode_version}")
    print(f"// with ICU {icu_version}, UCPTRIE_TYPE_FAST, UCPTRIE_VALUE_BITS_8")
    print(f"// {len(data)} bytes")
    print("// clang-format off")
    print("static constexpr uint8_t s_ucpTrieData[] = {", end="")
    for i in range(0, len(data)):
        v = data[i]
        prefix = ""
        if i % 64 == 0 and i != len(data) - 1:
            prefix = "\n    "
        print(f"{prefix}0x{v:02x},", end="")
    print("\n};")
    print("// clang-format on")


if __name__ == '__main__':
    main()
