= Lorem Ipsum Overview
<lorem-ipsum-overview>
Lorem ipsum dolor sit amet, #emph[consectetur] adipiscing elit, sed do
#strong[eiusmod] tempor incididunt ut labore et `dolore_magna` aliqua.
See the #link("https://example.com/docs")[example site] for more.

== Lists and Structure
<lists-and-structure>
Ut enim ad minim veniam:

- First bullet item
- Second bullet item
  - Nested alpha
  - Nested beta
- Third bullet with #strike[struck] text

+ Step one
+ Step two
+ Step three

/ Term One: Definition of the first term.

== Table Section
<table-section>
#figure(
  align(center)[#table(
    columns: 3,
    align: (left,right,center,),
    table.header([Name], [Quantity], [Price],),
    table.hline(),
    [Apple], [3], [1.25],
    [Banana], [12], [0.50],
    [Cherry], [100], [9.99],
  )]
  , kind: table
  )

== Code and Quotes
<code-and-quotes>
```python
def greet(name):
    return "Hello, " + name
```

#quote(block: true)[
Duis aute irure dolor in reprehenderit in voluptate velit esse cillum
dolore eu fugiat nulla pariatur.
]

=== Unicode Corner
<unicode-corner>
Accents: café, naïve, Ærøskøbing. Greek: αβγδ. Chinese: 汉字测试.
Hebrew: שלום עולם. Arabic: مرحبا بالعالم. Emoji: 🙂🚀.

Footnote reference here.#footnote[This is the footnote text.]
Superscript x#super[2] and subscript H#sub[2]O.

#figure(image("pixel.png", alt: "Tiny pixel"),
  caption: [
    Tiny pixel
  ]
)

#divider()

Final paragraph with a #link("https://example.org/page")[second link]
and an autolink #link("https://example.net/").
