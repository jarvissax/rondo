package com.jarvissax.rondo.ui

import android.content.Context
import android.graphics.Paint
import android.graphics.Typeface
import android.widget.TextView

// The app's one typeface: a newer Roboto bundled in assets (Azerbaijani "ə" fell back to
// another font). Android 4.2's system Roboto has no ə/Ə; this one covers it and
// all of Turkish. It ships in Regular only, so bold is Android's synthetic bold.
object Fonts {
    lateinit var regular: Typeface
        private set

    fun init(context: Context) {
        regular = runCatching { Typeface.createFromAsset(context.assets, "fonts/Roboto-Regular.ttf") }.getOrDefault(Typeface.DEFAULT)
    }
}

// Bold is drawn as the glyphs plus a thin outline of the same color (6 % of the text size):
// setTypeface(tf, BOLD) reports bold for an asset font without drawing it, and 4.2's hardware
// text renderer ignores setFakeBoldText. Set the text size first.
fun TextView.font(bold: Boolean = false) {
    if (typeface !== Fonts.regular) typeface = Fonts.regular
    paint.embolden(bold)
    invalidate()
}

fun Paint.font(bold: Boolean = false) {
    typeface = Fonts.regular
    embolden(bold)
}

private fun Paint.embolden(bold: Boolean) {
    style = if (bold) Paint.Style.FILL_AND_STROKE else Paint.Style.FILL
    strokeWidth = if (bold) textSize * 0.06f else 0f
}
