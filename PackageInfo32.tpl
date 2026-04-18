name			$(NAME)
version			$(VERSION)-1
architecture	$(ARCH)
summary		"Rakarrack is a richly featured multi-effects processor emulating a guitar effects pedalboard."
description	"Effects include compressor, expander, noise gate, graphic equalizer, parametric equalizer, exciter, shuffle, convolotron, valve, flanger, dual flange, chorus, musicaldelay, arpie, echo with reverse playback, musical delay, reverb, digital phaser, analogic phaser, synthfilter, varyband, ring, wah-wah, alien-wah, mutromojo, harmonizer, looper and four flexible distortion modules including sub-octave modulation and dirty octave up. Most of the effects engine is built from modules found in the excellent software synthesizer. ZynAddSubFX Presets and user interface are optimized for guitar, but Rakarrack processes signals in stereo while it does not apply internal band-limiting filtering, and thus is well suited to all musical instruments and vocals."
packager		"ablyss <jb@epluribusunix.net>"
vendor			"Rakarrack project"
licenses {
	"GNU GPL v2"
}
copyrights {
	"$(YEAR) Rakarrack project"
}
provides {
	$(NAME) = $(VERSION)-1
}
requires {
	haiku
	fltk_x86
	freetype_x86
	libxfont2_x86
	libsndfile_x86
	libsamplerate_x86
	libxpm_x86
}	
urls {
	"https://github.com/ablyssx74/rakarrack-haiku-32bit-hybrid"
}
source-urls {
# Download
	"https://github.com/ablyssx74/rakarrack-haiku-32bit-hybrid/archive/refs/tags/v1.0.3.tar.gz"
}
