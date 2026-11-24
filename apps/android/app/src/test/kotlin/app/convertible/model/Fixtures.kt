package app.convertible.model

/** Catalogue JSON shaped exactly like the core's output (see convertible.h), with formats from several families. */
object Fixtures {
    val FORMATS_JSON = """
        [
          {"id":"pdf","name":"PDF document","description":"","category":"document","extensions":["pdf"],"mime":"application/pdf","readable":false,"writable":false,"popularity":98},
          {"id":"png","name":"PNG image","description":"Lossless","category":"image","extensions":["png"],"mime":"image/png","readable":true,"writable":true,"popularity":95},
          {"id":"jpg","name":"JPEG image","description":"","category":"image","extensions":["jpg","jpeg"],"mime":"image/jpeg","readable":true,"writable":true,"popularity":95},
          {"id":"webp","name":"WebP image","description":"","category":"image","extensions":["webp"],"mime":"image/webp","readable":true,"writable":true,"popularity":80},
          {"id":"zip","name":"ZIP archive","description":"","category":"archive","extensions":["zip"],"mime":"application/zip","readable":true,"writable":true,"popularity":90},
          {"id":"txt","name":"Plain text","description":"","category":"document","extensions":["txt","text","log"],"mime":"text/plain","readable":true,"writable":true,"popularity":90},
          {"id":"base64","name":"Base64 text","description":"","category":"encoding","extensions":["b64","base64"],"mime":"text/plain","readable":true,"writable":true,"popularity":60},
          {"id":"hex","name":"Hexadecimal text","description":"","category":"encoding","extensions":["hex"],"mime":"text/plain","readable":true,"writable":true,"popularity":50},
          {"id":"checksums","name":"Checksum report","description":"","category":"encoding","extensions":["txt","checksums"],"mime":"text/plain","readable":false,"writable":true,"popularity":40},
          {"id":"gif","name":"GIF image","description":"","category":"image","extensions":["gif"],"mime":"image/gif","readable":true,"writable":true,"popularity":70},
          {"id":"bmp","name":"BMP image","description":"","category":"image","extensions":["bmp"],"mime":"image/bmp","readable":true,"writable":true,"popularity":30},
          {"id":"futurefmt","name":"Future thing","description":"","category":"hologram","extensions":["fut"],"mime":"x/fut","readable":true,"writable":false,"popularity":1}
        ]
    """.trimIndent()

    /** Targets for a PNG source, most relevant first (same category, lossless, popular), with the compress entry. */
    val PNG_TARGETS_JSON = """
        [
          {"id":"png","name":"PNG image","category":"image","extensions":["png"],"mime":"image/png","lossy":false,"compress":true},
          {"id":"webp","name":"WebP image","category":"image","extensions":["webp"],"mime":"image/webp","lossy":false,"compress":false},
          {"id":"bmp","name":"BMP image","category":"image","extensions":["bmp"],"mime":"image/bmp","lossy":false,"compress":false},
          {"id":"gif","name":"GIF image","category":"image","extensions":["gif"],"mime":"image/gif","lossy":true,"compress":false},
          {"id":"jpg","name":"JPEG image","category":"image","extensions":["jpg","jpeg"],"mime":"image/jpeg","lossy":true,"compress":false},
          {"id":"pdf","name":"PDF document","category":"document","extensions":["pdf"],"mime":"application/pdf","lossy":false,"compress":false},
          {"id":"zip","name":"ZIP archive","category":"archive","extensions":["zip"],"mime":"application/zip","lossy":false,"compress":false},
          {"id":"base64","name":"Base64 text","category":"encoding","extensions":["b64","base64"],"mime":"text/plain","lossy":false,"compress":false},
          {"id":"hex","name":"Hexadecimal text","category":"encoding","extensions":["hex"],"mime":"text/plain","lossy":false,"compress":false},
          {"id":"checksums","name":"Checksum report","category":"encoding","extensions":["txt","checksums"],"mime":"text/plain","lossy":false,"compress":false}
        ]
    """.trimIndent()

    val JPG_SCHEMA_JSON = """
        [
          {"key":"quality","label":"Quality","type":"int","default":90,"min":1,"max":100,"choices":[],"description":"Higher is better and larger.","category":"basic"},
          {"key":"preset","label":"Preset","type":"enum","default":"balanced","min":null,"max":null,"choices":["high","balanced","small"],"description":"","category":"basic"},
          {"key":"strip_metadata","label":"Strip metadata","type":"bool","default":true,"min":null,"max":null,"choices":[],"description":"","category":"advanced"},
          {"key":"scale","label":"Scale","type":"float","default":1.0,"min":0.01,"max":1,"choices":[],"description":"","category":"advanced"},
          {"key":"comment","label":"Comment","type":"string","default":"","min":null,"max":null,"choices":[],"description":"","category":"advanced"}
        ]
    """.trimIndent()
}
