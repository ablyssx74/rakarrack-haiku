name			$(NAME)
version			$(VERSION)-$(REVISION)
architecture	$(ARCH)
summary		"Rakarrack is a richly featured multi-effects processor emulating a guitar effects pedalboard."
description	"Effects include compressor, expander, noise gate, graphic equalizer, parametric equalizer, exciter, shuffle, convolotron, valve, flanger, dual flange, chorus, musicaldelay, arpie, echo with reverse playback, musical delay, reverb, digital phaser, analogic phaser, synthfilter, varyband, ring, wah-wah, alien-wah, mutromojo, harmonizer, looper and four flexible distortion modules including sub-octave modulation and dirty octave up. Most of the effects engine is built from modules found in the excellent software synthesizer. ZynAddSubFX Presets and user interface are optimized for guitar, but Rakarrack processes signals in stereo while it does not apply internal band-limiting filtering, and thus is well suited to all musical instruments and vocals."
packager		"ablyss <rakarrack@epluribusunix.net>"
vendor			"Rakarrack project"
licenses {
	"GNU GPL v2"
}
copyrights {
	"$(YEAR) Rakarrack project"
}
provides {
	$(NAME) = $(VERSION)-$(REVISION)
}
requires {
	haiku
    fltk$(is32bit)
	fontconfig$(is32bit)
	freetype$(is32bit)
	libxfont2$(is32bit)
	libsndfile$(is32bit)
	libsamplerate$(is32bit)
	libxpm$(is32bit)
	lib:libcurl$(is32bit)
}	
urls {
	"https://github.com/ablyssx74/rakarrack-haiku"
}
source-urls {
# Download
	"https://github.com/ablyssx74/rakarrack-haiku/archive/refs/tags/v1.0.0.tar.gz"
}
