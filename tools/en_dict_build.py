"""영어 오프라인 사전(en-dict.tsv) 만들기.

입력 (kaikki.org 에서 받는다, 위키낱말사전 데이터, CC BY-SA 4.0):
  ko-extract.jsonl.gz                 https://kaikki.org/dictionary/downloads/ko/ko-extract.jsonl.gz
                                      위키낱말사전(한국어판). 영어 표제어의 한국어 풀이를 쓴다.
  kaikki.org-dictionary-English.jsonl.gz  https://kaikki.org/dictionary/English/kaikki.org-dictionary-English.jsonl.gz
                                      영어 위키낱말사전의 영어 단어. 한국어 번역 · 영어 정의 · IPA · 변화형을 쓴다.

사용: python tools/en_dict_build.py <ko-extract.jsonl.gz> <English.jsonl.gz> <출력 폴더>
출력: <출력 폴더>/en-dict.tsv (+ LICENSE.txt). 형식은 src/endict.cpp 주석 참고.
버전(VERSION)을 바꾸면 src/endict.cpp 의 kVersion 과 EnDict::packUrl 도 함께 바꾸고 새 릴리스를 올린다.
"""
import gzip
import json
import os
import re
import sys

VERSION = 1

POS_KO = {
    "noun": "명사", "verb": "동사", "adj": "형용사", "adv": "부사", "pron": "대명사", "prep": "전치사",
    "conj": "접속사", "intj": "감탄사", "det": "한정사", "article": "관사", "name": "고유명사", "num": "수사",
    "particle": "불변화사", "contraction": "축약형", "phrase": "구", "prep_phrase": "전치사구", "prefix": "접두사",
    "suffix": "접미사", "abbrev": "약어", "symbol": "기호", "proverb": "속담", "postp": "후치사",
}
SKIP_POS = {"prefix", "suffix", "infix", "interfix", "affix", "circumfix", "character", "symbol", "punct"}
BAD_SENSE_TAGS = {"obsolete", "archaic", "rare", "dialectal", "nonstandard", "misspelling", "dated", "Scotland",
                  "Northern-England", "Ireland", "historical"}
FORM_NOTES = [  # (태그 집합, 설명) 앞에서부터 맞는 것
    ({"past", "participle"}, "과거분사"), ({"past"}, "과거형"), ({"present", "participle"}, "-ing 형"),
    ({"third-person", "singular", "present"}, "3인칭 단수 현재형"), ({"plural"}, "복수형"),
    ({"comparative"}, "비교급"), ({"superlative"}, "최상급"),
]
FORM_SKIP_TAGS = {"alternative", "obsolete", "archaic", "nonstandard", "dialectal", "table-tags", "inflection-template",
                  "rare", "misspelling", "romanization", "abbreviation"}

# 뜻풀이가 다른 항목을 가리키기만 하는 정의 (약어, 다른 표기 …) 는 뺀다
SKIP_GLOSS = re.compile(r"^(initialism|abbreviation|acronym|alternative (form|spelling)|misspelling|obsolete|archaic|eye dialect|pronunciation spelling|clipping|ellipsis|contraction) of\b", re.I)

MAX_EN_PER_POS = 3
MAX_KO_PER_POS = 6


def form_note(tags):
    t = set(tags)
    for need, note in FORM_NOTES:
        if need <= t:
            return note
    return None


def clean_ipa(s):
    s = s.strip().strip("/[]").strip()
    return s if s and len(s) < 40 else ""


def clean_text(s, limit):
    s = re.sub(r"\s+", " ", s).strip()
    s = s.replace("\t", " ")
    if len(s) > limit:
        s = s[: limit - 1].rstrip() + "…"
    return s


def clean_ko_gloss(s):
    s = re.sub(r"\(부록[^)]*\)", "", s)
    s = re.sub(r"\s+", " ", s).strip().rstrip(".").strip()
    return clean_text(s, 80)


def pick_ipa(sounds):
    best = ""
    for snd in sounds or []:
        ipa = snd.get("ipa")
        if not ipa or ipa.startswith("["):
            continue
        ipa = clean_ipa(ipa)
        if not ipa:
            continue
        tags = set(snd.get("tags", [])) | set(snd.get("raw_tags", []))
        if tags & {"US", "General-American", "GA"}:
            return ipa
        if not best:
            best = ipa
    return best


def is_word(w):
    return bool(w) and " " not in w and len(w) <= 30 and re.fullmatch(r"[A-Za-z][A-Za-z'\-\.]*", w) is not None


def main():
    ko_path, en_path, out_dir = sys.argv[1:4]
    os.makedirs(out_dir, exist_ok=True)

    # 1) 한국어판: 영어 표제어의 한국어 풀이
    ko = {}      # key -> [(품사, [뜻])]
    ko_ipa = {}
    with gzip.open(ko_path, "rt", encoding="utf-8") as f:
        for line in f:
            d = json.loads(line)
            if d.get("lang_code") != "en":
                continue
            w = d.get("word", "")
            if not is_word(w):
                continue
            key = w.lower()
            glosses = []
            for s in d.get("senses", []):
                for g in s.get("glosses", [])[-1:]:
                    g = clean_ko_gloss(g)
                    if g and g not in glosses:
                        glosses.append(g)
            if not glosses:
                continue
            pos = d.get("pos_title") or POS_KO.get(d.get("pos", ""), "")
            if pos in ("unknown", "미분류"):
                pos = ""
            ko.setdefault(key, []).append((pos, glosses[:MAX_KO_PER_POS]))
            ipa = pick_ipa(d.get("sounds"))
            if ipa and key not in ko_ipa:
                ko_ipa[key] = ipa
    print("ko entries", len(ko), file=sys.stderr)

    # 2) 영어판
    lemmas = {}  # key -> {"ipa": str, "blocks": {pos: {"ko": [], "en": []}}, "order": [pos]}
    forms = {}   # form -> [(lemma, note)]
    n = 0
    with gzip.open(en_path, "rt", encoding="utf-8") as f:
        for line in f:
            n += 1
            if n % 200000 == 0:
                print("  ", n, file=sys.stderr)
            d = json.loads(line)
            w = d.get("word", "")
            pos = d.get("pos", "")
            if not is_word(w) or pos in SKIP_POS:
                continue
            key = w.lower()
            senses = d.get("senses", [])

            # 변화형 표
            for fm in d.get("forms", []):
                form = fm.get("form", "")
                tags = fm.get("tags", [])
                if not is_word(form) or form.lower() == key or set(tags) & FORM_SKIP_TAGS:
                    continue
                note = form_note(tags)
                if note:
                    forms.setdefault(form.lower(), [])
                    if (key, note) not in forms[form.lower()]:
                        forms[form.lower()].append((key, note))

            # 변화형만 있는 항목 (gave: "simple past of give") → 변화형 표로만
            real = [s for s in senses if not s.get("form_of") and not s.get("alt_of") and not {"form-of", "alt-of"} & set(s.get("tags", []))]
            for s in senses:
                for fo in s.get("form_of", []):
                    lem = fo.get("word", "")
                    note = form_note(s.get("tags", []))
                    if is_word(lem) and note and lem.lower() != key:
                        forms.setdefault(key, [])
                        if (lem.lower(), note) not in forms[key]:
                            forms[key].append((lem.lower(), note))
            if not real:
                continue

            en_gl = []
            for s in real:
                if set(s.get("tags", [])) & BAD_SENSE_TAGS:
                    continue
                gl = s.get("glosses", [])
                if not gl:
                    continue
                g = clean_text(gl[-1], 140)
                if SKIP_GLOSS.match(g):
                    continue
                if g and g not in en_gl:
                    en_gl.append(g)
                if len(en_gl) >= MAX_EN_PER_POS:
                    break
            ko_tr = []
            for t in d.get("translations", []):
                if t.get("lang_code") != "ko" and t.get("code") != "ko":
                    continue
                tw = clean_text(t.get("word", ""), 40)
                if tw and tw not in ko_tr:
                    ko_tr.append(tw)
            if not en_gl and not ko_tr:
                continue
            ent = lemmas.setdefault(key, {"ipa": "", "blocks": {}, "order": [], "proper": True})
            if not ent["ipa"]:
                ent["ipa"] = pick_ipa(d.get("sounds"))
            if pos != "name":
                ent["proper"] = False
            pk = POS_KO.get(pos, pos)
            b = ent["blocks"].setdefault(pk, {"ko": [], "en": []})
            if pk not in ent["order"]:
                ent["order"].append(pk)
            for x in ko_tr:
                if x not in b["ko"] and len(b["ko"]) < MAX_KO_PER_POS:
                    b["ko"].append(x)
            for x in en_gl:
                if x not in b["en"] and len(b["en"]) < MAX_EN_PER_POS:
                    b["en"].append(x)
    print("en lines", n, "lemmas", len(lemmas), "forms", len(forms), file=sys.stderr)

    # 3) 합치기: 한국어판 풀이를 같은 품사 묶음 앞에 둔다
    for key, items in ko.items():
        ent = lemmas.setdefault(key, {"ipa": "", "blocks": {}, "order": [], "proper": False})
        ent["proper"] = False
        for pos, gl in items:
            target = pos if pos in ent["blocks"] else None
            if target is None:  # 품사 이름이 다르면 (자동사/타동사 → 동사 등) 비슷한 묶음으로
                for p in ent["order"]:
                    if pos and (pos in p or p in pos or (pos.endswith("동사") and p == "동사")):
                        target = p
                        break
            if target is None:
                target = pos or (ent["order"][0] if ent["order"] else "")
                ent["blocks"].setdefault(target, {"ko": [], "en": []})
                if target not in ent["order"]:
                    ent["order"].insert(0, target)
            b = ent["blocks"][target]
            # 위키낱말사전(한국어판) 풀이 4개 + 영어판 번역(대개 대표 뜻) 을 섞는다
            mixed = gl[:4] + [x for x in b["ko"] if x not in gl[:4]]
            b["ko"] = mixed[:MAX_KO_PER_POS]
        if not ent["ipa"]:
            ent["ipa"] = ko_ipa.get(key, "")

    # 4) 어떤 표제어를 넣을까: 한국어 뜻이 있거나, 발음 기호가 있는 (= 어느 정도 쓰이는) 소문자 단어,
    #    그리고 변화형 표가 가리키는 표제어
    def has_ko(e):
        return any(b["ko"] for b in e["blocks"].values())

    keep = set()
    for key, e in lemmas.items():
        if has_ko(e) or (e["ipa"] and not e["proper"]):
            keep.add(key)
        elif e["proper"] and e["ipa"]:
            keep.add(key)  # 인명 · 지명 (Neal 등)
    for form, lst in forms.items():
        for lem, _ in lst:
            if lem in lemmas and lemmas[lem]["ipa"]:
                keep.add(lem)

    out = os.path.join(out_dir, "en-dict.tsv")
    nl = nf = 0
    with open(out, "w", encoding="utf-8", newline="\n") as o:
        o.write(f"#YSED\t{VERSION}\n")
        for key in sorted(keep):
            e = lemmas[key]
            blocks = []
            # 한국어 뜻이 있는 품사 먼저, 고유명사는 맨 뒤 (다른 품사가 있으면)
            order = sorted(e["order"], key=lambda p: (p == "고유명사", not e["blocks"][p]["ko"], e["order"].index(p)))
            for p in order:
                b = e["blocks"][p]
                if not b["ko"] and not b["en"]:
                    continue
                en = b["en"] if len(b["ko"]) < 3 else b["en"][:1]  # 한국어 뜻이 넉넉하면 영어 정의는 하나만
                blocks.append(p + "\x1f" + "\x1d".join(b["ko"]) + "\x1f" + "\x1d".join(en))
            if not blocks:
                continue
            o.write("L\t" + key + "\t" + e["ipa"] + "\t" + "\x1e".join(blocks) + "\n")
            nl += 1
        for form in sorted(forms):
            for lem, note in forms[form]:
                if lem in keep:
                    o.write("F\t" + form + "\t" + lem + "\t" + note + "\n")
                    nf += 1
    with open(os.path.join(out_dir, "LICENSE.txt"), "w", encoding="utf-8") as o:
        o.write(
            "en-dict.tsv 는 위키낱말사전(Wiktionary, 영어판 · 한국어판)의 내용을 kaikki.org (Wiktextract, Tatu Ylonen) 의\n"
            "추출 데이터에서 가공한 것입니다. 라이선스: Creative Commons Attribution-ShareAlike 4.0 (CC BY-SA 4.0)\n"
            "https://creativecommons.org/licenses/by-sa/4.0/\n"
            "원본: https://en.wiktionary.org , https://ko.wiktionary.org , https://kaikki.org\n")
    print("wrote", out, "lemmas", nl, "forms", nf, "bytes", os.path.getsize(out), file=sys.stderr)


if __name__ == "__main__":
    main()
