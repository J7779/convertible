"""Writes core/tests/data/encoding/charsets/: sample texts encoded with Python's codecs."""
import os

OUT = os.path.dirname(os.path.abspath(__file__))
os.makedirs(OUT, exist_ok=True)
TEXTS = {
    "ja": "日本語のテキストです。カタカナとひらがな、漢字を含みます。\n数字１２３と記号「」も使います。\n",
    "ja_halfwidth": "半角ｶﾀｶﾅと全角カタカナ。\n",
    "zh_cn": "这是一个简体中文的测试文本，包含常用汉字和标点符号。\n第二行：数字一二三。\n",
    "zh_tw": "這是一個繁體中文的測試文本，包含常用漢字和標點符號。\n第二行：數字一二三。\n",
    "ko": "이것은 한국어 테스트 문장입니다. 한글과 숫자 123을 포함합니다.\n",
    "ru": "Съешь же ещё этих мягких французских булок, да выпей чаю.\n",
    "el": "Ξεσκεπάζω την ψυχοφθόρα βδελυγμία.\n",
    "pl": "Zażółć gęślą jaźń. Příliš žluťoučký kůň úpěl ďábelské ódy.\n",
    "fr": "Portez ce vieux whisky au juge blond qui fume : é à ç ù ï.\n",
    "he": "דג סקרן שט בים מאוכזב ולפתע מצא חברה.\n",
}
FILES = [
    ("shift_jis", "ja"), ("cp932", "ja_halfwidth"), ("euc_jp", "ja"), ("iso2022_jp", "ja"), ("gbk", "zh_cn"),
    ("gb18030", "zh_cn"), ("big5", "zh_tw"), ("euc_kr", "ko"), ("koi8_r", "ru"), ("cp1251", "ru"), ("cp866", "ru"),
    ("iso8859_5", "ru"), ("cp1253", "el"), ("iso8859_7", "el"), ("cp1250", "pl"), ("iso8859_2", "pl"), ("cp852", "pl"),
    ("mac_roman", "fr"), ("cp850", "fr"), ("cp437", "fr"), ("cp037", "fr"), ("cp500", "fr"), ("cp1255", "he"),
    ("iso8859_15", "fr"),
]
lines = []
for codec, key in FILES:
    data = TEXTS[key].encode(codec)
    open(os.path.join(OUT, "%s.txt" % codec), "wb").write(data)
    lines.append("%s %s" % (codec, key))
for key, text in TEXTS.items():
    open(os.path.join(OUT, "%s.utf8" % key), "w", encoding="utf-8", newline="").write(text)
open(os.path.join(OUT, "index.txt"), "w").write("\n".join(lines) + "\n")
print(len(FILES), "files")
