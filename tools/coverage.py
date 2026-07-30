#!/usr/bin/env python3
"""Physics coverage audit: Melee decomp vs tier-fighter.

Extracts every enumerable physics surface from the decomp and diffs it against our
config, so "have we covered everything?" is a command rather than a judgment call.

The point is that hand-rolled greps fail SILENTLY -- a mistyped awk range returns
zero matches and looks like "nothing there" rather than "the query broke". Every
extractor here asserts a non-zero, plausible count and dies loudly otherwise.

Usage:
    tools/coverage.py --decomp /tmp/melee-ref [--verbose]

Exit status is 0 always: this is a report, not a gate. Nothing here should fail a
build, because most "uncovered" items are deliberate omissions (items, bosses).
"""

import argparse
import json
import os
import re
import sys

# --- Surfaces ---------------------------------------------------------------
# Each surface is an enumerable list in the decomp that we can diff against.
# Any surface we cannot enumerate is reported as such rather than skipped, because
# a silently-missing surface is how the first audit missed 501 global constants.


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def struct_body(text, header_regex, name):
    """Return the body of a struct, or raise if the pattern does not match.

    Raising matters: the original hand-rolled awk range silently returned nothing
    for ftCommonData, which read as "no fields" instead of "broken query".
    """
    m = re.search(header_regex, text)
    if not m:
        raise LookupError(f"could not locate struct {name} (regex: {header_regex})")
    start = m.end()
    depth = 1
    i = start
    while i < len(text) and depth > 0:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    if depth != 0:
        raise LookupError(f"unbalanced braces reading struct {name}")
    return text[start : i - 1]


FIELD_RE = re.compile(
    r"^\s*(?:/\*[^*]*\*/\s*)?"                     # optional offset comment
    r"(?:const\s+)?"
    r"(float|double|int|s8|u8|s16|u16|s32|u32|bool|Vec2|Vec3|enum_t|"
    r"[A-Za-z_][A-Za-z0-9_]*_t)\s+"                # type
    r"([A-Za-z_][A-Za-z0-9_]*)"                    # name
    r"\s*(?:\[[^\]]*\])?\s*(?::\s*\d+\s*)?;",      # optional array / bitfield
    re.MULTILINE,
)


def extract_fields(body):
    """All (type, name) pairs in a struct body, skipping nested struct contents."""
    out = []
    for m in FIELD_RE.finditer(body):
        out.append((m.group(1), m.group(2)))
    return out


def is_placeholder(name):
    """True for decomp placeholder names like x1F0 -- unnamed, so unknowable."""
    return bool(re.fullmatch(r"x[0-9A-Fa-f]+", name))


def semantic(name):
    """Strip the decomp's offset prefix: x260_startShieldHealth -> startShieldHealth.

    Fields like this are PARTIALLY named -- the offset prefix is noise but the
    suffix tells us what it is, so they count as knowable.
    """
    m = re.fullmatch(r"x[0-9A-Fa-f]+_(.+)", name)
    return m.group(1) if m else name


def classify(name):
    """knowable | placeholder -- can we tell what this field is for?"""
    if is_placeholder(name):
        return "placeholder"
    return "knowable"


# --- Domain tagging ---------------------------------------------------------
# Tag each knowable field by subsystem, so the report says WHERE we are thin
# rather than just how many fields exist.

DOMAINS = [
    ("walk",      r"walk"),
    ("dash/run",  r"dash|run(?!_anim)|brake"),
    ("jump",      r"jump|hop|kneebend"),
    ("air",       r"aerial|air_|drift|grav|terminal|fall"),
    ("friction",  r"friction"),
    ("knockback", r"\bkb_|knockback|hit_weight|hitlag|hitstun"),
    ("shield",    r"shield|guard"),
    ("ledge",     r"ledge|cliff"),
    ("landing",   r"landing"),
    ("attack",    r"attack|jab|smash|swing"),
    ("grab",      r"grab|catch|throw|captur|shoulder|bury"),
    ("tech",      r"passive|tech|down|bound"),
    ("sdi",       r"sdi"),
    ("wall/ceil", r"wall|ceil"),
    ("input",     r"stick|tap_|threshold|window|deadzone"),
    ("status",    r"ice|song|bind|sleep|furafura|metal|poison|mushroom|kinoko"),
    ("item",      r"item|parasol|gun|flower|scope|barrel|star|hammer|screw|lipstick|"
                  r"harisen|bat_|sword"),
    ("cosmetic",  r"camera|trophy|model_scal|name_tag|anim_rate|scale|radians|zoom|"
                  r"respawn_platform|sfx"),
    ("weight",    r"weight"),
]


def domain_of(name):
    low = semantic(name).lower()
    for dom, pat in DOMAINS:
        if re.search(pat, low):
            return dom
    return "unclassified"


# --- Our side ---------------------------------------------------------------
# Everything tunable in our sim lives in config.h, which is exactly why that rule
# exists: it makes our half of this diff mechanically enumerable too.


def our_config_identifiers(config_path):
    text = read(config_path)
    names = set()
    # struct field declarations
    for m in re.finditer(
        r"^\s*(?:fx|int|int8_t|uint8_t|uint16_t|bool|float)\s+"
        r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*=",
        text,
        re.MULTILINE,
    ):
        names.add(m.group(1))
    # enum members and constants
    for m in re.finditer(r"^\s*(ATK_[A-Z_]+|CHAR_[A-Z_]+|k[A-Za-z0-9_]+)", text,
                         re.MULTILINE):
        names.add(m.group(1))
    return names


def our_states(state_path):
    text = read(state_path)
    body = struct_body(text, r"enum class ActionState\s*:\s*uint8_t\s*\{",
                       "ActionState")
    return [m.group(1) for m in re.finditer(r"^\s*([A-Z][A-Za-z0-9]*)\s*,", body,
                                            re.MULTILINE)]


# Normalize both sides to a comparable token so snake_case and camelCase match.
def norm(name):
    s = semantic(name).lower()
    return re.sub(r"[^a-z0-9]", "", s)


def build_our_index(names):
    """Map normalized -> original for fuzzy matching."""
    return {norm(n): n for n in names}


# Manual mapping for cases where the names genuinely differ but the concept is
# covered. Kept explicit and small: every entry is a claim that needs to be true.
EQUIVALENT = {
    "walkmaxvel": "walkSpeed",
    "dashrunterminalvelocity": "dashSpeed",
    "grfriction": "friction",
    "jumpstartuptime": "jumpsquatFrames",
    "jumpvinitialvelocity": "fullVelocity",
    "hopvinitialvelocity": "hopVelocity",
    "airjumpvmultiplier": "airJumpVelocity",
    "maxjumps": "maxAirJumps",
    "grav": "gravity",
    "terminalvel": "termVelocity",
    "fastfallvelocity": "fastFallSpeed",
    "aerialfriction": "friction",
    "airdriftmax": "maxSpeed",
    "aerialdriftbase": "acceleration",
    "weight": "weight",
    "normallandinglag": "aerialLagFrames",
    "ledgejumphorizontalvelocity": "jumpVelX",
    "ledgejumpverticalvelocity": "jumpVelY",
    "ledgecooldown": "cooldownFrames",
    "kbmin": "floor",
    "knockbackframedecay": "airDecay",
    "startshieldhealth": None,          # shield not implemented
    "tapjumpthreshold": None,
    "tapjumpreleasethreshold": None,
    "sdiminstickmag": None,
    "sdiposscale": None,
    "sdistickwindow": None,
    "kbsquatmul": None,
    "kbsmashchargemul": "fullChargeKnockbackMult",
    "hitweightmul": None,
    "escapeairdecay": "momentumDecay",
    "escapeairforce": "speed",

    # --- Closed in the gap-closure pass -----------------------------------
    "airmaxhorizontalvelocity": "maxHorizontal",
    "airdriftstickmul": "stickMul",
    "landingairnlag": "landingLag",     # per-attack now, not one shared value
    "landingairflag": "landingLag",
    "landingairblag": "landingLag",
    "landingairhilag": "landingLag",
    "landingairlwlag": "landingLag",
    "unkhitlagframes": "maxFrames",     # x194_unkHitLagFrames -> Hitlag::maxFrames
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--decomp", default="/tmp/melee-ref")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    ft = os.path.join(args.decomp, "src/melee/ft")
    types_h = os.path.join(ft, "types.h")
    if not os.path.isfile(types_h):
        print(f"ERROR: decomp not found at {args.decomp}", file=sys.stderr)
        print("  clone it: git clone --depth 1 "
              "https://github.com/doldecomp/melee.git /tmp/melee-ref",
              file=sys.stderr)
        return 1

    text = read(types_h)
    report = {}

    # --- Surface 1 & 2: the two attribute structs --------------------------
    surfaces = {
        "per-character attrs (ftCo_DatAttrs)":
            (r"typedef struct ftCo_DatAttrs\s*\{", 60),
        "global constants (ftCommonData)":
            (r"struct ftCommonData\s*\{", 400),
    }

    our_names = our_config_identifiers(os.path.join(args.root, "src/sim/config.h"))
    our_idx = build_our_index(our_names)

    for label, (regex, min_expected) in surfaces.items():
        body = struct_body(text, regex, label)
        fields = extract_fields(body)
        # Guard: a broken regex must fail loudly, not report zero.
        assert len(fields) >= min_expected, (
            f"{label}: extracted only {len(fields)} fields, expected >= "
            f"{min_expected}. The extractor is probably broken."
        )

        knowable = [n for _, n in fields if classify(n) == "knowable"]
        placeholders = [n for _, n in fields if classify(n) == "placeholder"]

        covered, missing = [], []
        for n in knowable:
            key = norm(n)
            if key in our_idx:
                covered.append((n, our_idx[key]))
            elif key in EQUIVALENT:
                if EQUIVALENT[key] is None:
                    missing.append((n, domain_of(n), "known gap"))
                else:
                    covered.append((n, EQUIVALENT[key]))
            else:
                missing.append((n, domain_of(n), "unmapped"))

        report[label] = {
            "total": len(fields),
            "knowable": len(knowable),
            "placeholders": len(placeholders),
            "covered": len(covered),
            "missing": len(missing),
            "missing_detail": missing,
            "covered_detail": covered,
        }

    # --- Surface 3: motion states ------------------------------------------
    fwd = os.path.join(ft, "chara/ftCommon/forward.h")
    fwd_text = read(fwd)
    m = re.search(r"ftCo_MS_None\s*=\s*-1,", fwd_text)
    assert m, "could not find the motion-state enum start"
    tail = fwd_text[m.end():]
    end = tail.index("ftCo_MS_Count")
    their_states = re.findall(r"ftCo_MS_([A-Za-z0-9]+)", tail[:end])
    assert len(their_states) > 300, (
        f"only found {len(their_states)} motion states, expected >340"
    )
    ours = our_states(os.path.join(args.root, "src/sim/state.h"))
    report["motion states"] = {
        "theirs": len(their_states),
        "ours": len(ours),
        "our_states": ours,
    }

    # --- Surface 4: physics functions --------------------------------------
    common_c = read(os.path.join(ft, "ftcommon.c"))
    fns = re.findall(r"^(?:void|float|bool|int|HSD_GObj\*)\s+(ftCommon_[A-Za-z0-9_]+)",
                     common_c, re.MULTILINE)
    named_fns = [f for f in fns if not re.fullmatch(r"ftCommon_[0-9A-F]+", f)]
    report["ftcommon.c functions"] = {
        "total": len(fns),
        "named": len(named_fns),
        "named_list": sorted(set(named_fns)),
    }

    # --- Report ------------------------------------------------------------
    if args.json:
        print(json.dumps(report, indent=2, default=str))
        return 0

    print("=" * 78)
    print("PHYSICS COVERAGE: Melee decomp vs tier-fighter")
    print("=" * 78)

    for label in surfaces:
        r = report[label]
        pct = (100 * r["covered"] // r["knowable"]) if r["knowable"] else 0
        print(f"\n## {label}")
        print(f"   {r['total']} fields total"
              f"  |  {r['knowable']} knowable"
              f"  |  {r['placeholders']} unnamed placeholders")
        print(f"   COVERED {r['covered']}/{r['knowable']} ({pct}%)")

        if r["missing"]:
            by_dom = {}
            for n, dom, why in r["missing_detail"]:
                by_dom.setdefault(dom, []).append(n)
            print(f"\n   Not covered, by subsystem:")
            for dom in sorted(by_dom, key=lambda d: -len(by_dom[d])):
                items = by_dom[dom]
                shown = ", ".join(items[:4])
                more = f" (+{len(items)-4})" if len(items) > 4 else ""
                print(f"     {dom:12} {len(items):3}  {shown}{more}")

        if args.verbose and r["covered_detail"]:
            print(f"\n   Covered:")
            for theirs, ours_name in sorted(r["covered_detail"]):
                print(f"     {theirs:44} -> {ours_name}")

    ms = report["motion states"]
    print(f"\n## motion states")
    print(f"   theirs {ms['theirs']}  |  ours {ms['ours']}")
    print(f"   (see STATES.md -- most of theirs collapse or are out of scope)")

    fn = report["ftcommon.c functions"]
    print(f"\n## ftcommon.c physics functions")
    print(f"   {fn['total']} total  |  {fn['named']} meaningfully named")

    print("\n" + "=" * 78)
    print("Placeholder fields (xNN) are unnamed in the decomp -- their purpose is")
    print("unknown, so they can be neither covered nor ruled out. They are excluded")
    print("from the percentages rather than counted as gaps.")
    print("=" * 78)
    return 0


if __name__ == "__main__":
    sys.exit(main())
