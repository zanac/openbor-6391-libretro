# OpenBOR v4.0 Build 8025 (DCurrent/openbor 787b6770, 2026-09-22)
E8025 := $(CORE_DIR)/engines/8025

ENGINE_8025_NAME  := v4.0 Build 8025
ENGINE_8025_FLAGS := -fcommon \
	-I$(E8025)/libretro \
	-I$(E8025) \
	-I$(E8025)/source \
	-I$(E8025)/source/adpcmlib \
	-I$(E8025)/source/gamelib \
	-I$(E8025)/source/gfxlib \
	-I$(E8025)/source/preprocessorlib \
	-I$(E8025)/source/ramlib \
	-I$(E8025)/source/randlib \
	-I$(E8025)/source/scriptlib \
	-I$(E8025)/source/openborscript \
	-I$(E8025)/source/pnglib \
	-I$(E8025)/source/tracelib2 \
	-I$(E8025)/source/webmlib

ENGINE_8025_SOURCES := \
	$(E8025)/openbor.c \
	$(E8025)/openborscript.c \
	$(E8025)/source/gamelib/anigif.c \
	$(E8025)/source/gamelib/bitmap.c \
	$(E8025)/source/gamelib/draw.c \
	$(E8025)/source/gamelib/draw16.c \
	$(E8025)/source/gamelib/draw32.c \
	$(E8025)/source/gamelib/filecache.c \
	$(E8025)/source/gamelib/font.c \
	$(E8025)/source/gamelib/loadimg.c \
	$(E8025)/source/gamelib/models.c \
	$(E8025)/source/gamelib/packfile.c \
	$(E8025)/source/gamelib/packfile_handle.c \
	$(E8025)/source/gamelib/palette.c \
	$(E8025)/source/gamelib/pixelformat.c \
	$(E8025)/source/gamelib/screen.c \
	$(E8025)/source/gamelib/screen16.c \
	$(E8025)/source/gamelib/screen32.c \
	$(E8025)/source/gamelib/sound_channel.c \
	$(E8025)/source/gamelib/sound_stream.c \
	$(E8025)/source/gamelib/soundmix.c \
	$(E8025)/source/gamelib/sprite.c \
	$(E8025)/source/gamelib/spriteq.c \
	$(E8025)/source/gamelib/spritex8p16.c \
	$(E8025)/source/gamelib/spritex8p32.c \
	$(E8025)/source/gamelib/transform.c \
	$(E8025)/source/gamelib/translation.c \
	$(E8025)/source/omath.c \
	$(E8025)/source/openborscript/animation.c \
	$(E8025)/source/openborscript/audio.c \
	$(E8025)/source/openborscript/axis.c \
	$(E8025)/source/openborscript/binding.c \
	$(E8025)/source/openborscript/collision_attack.c \
	$(E8025)/source/openborscript/collision_body.c \
	$(E8025)/source/openborscript/collision_coordinates.c \
	$(E8025)/source/openborscript/colorset.c \
	$(E8025)/source/openborscript/command_input.c \
	$(E8025)/source/openborscript/commands.c \
	$(E8025)/source/openborscript/common_property.c \
	$(E8025)/source/openborscript/constants.c \
	$(E8025)/source/openborscript/datetime.c \
	$(E8025)/source/openborscript/drawmethod.c \
	$(E8025)/source/openborscript/entity.c \
	$(E8025)/source/openborscript/faction.c \
	$(E8025)/source/openborscript/flash.c \
	$(E8025)/source/openborscript/global_config.c \
	$(E8025)/source/openborscript/icon.c \
	$(E8025)/source/openborscript/index.c \
	$(E8025)/source/openborscript/layer.c \
	$(E8025)/source/openborscript/level.c \
	$(E8025)/source/openborscript/math.c \
	$(E8025)/source/openborscript/model.c \
	$(E8025)/source/openborscript/movie.c \
	$(E8025)/source/openborscript/recursive_effect.c \
	$(E8025)/source/openborscript/spawn_hud.c \
	$(E8025)/source/openborscript/status_dial.c \
	$(E8025)/source/openborscript/string.c \
	$(E8025)/source/pnglib/pngdec.c \
	$(E8025)/source/pnglib/savepng.c \
	$(E8025)/source/preprocessorlib/pp_expr.c \
	$(E8025)/source/preprocessorlib/pp_lexer.c \
	$(E8025)/source/preprocessorlib/pp_parser.c \
	$(E8025)/source/ramlib/ram.c \
	$(E8025)/source/randlib/rand32.c \
	$(E8025)/source/scriptlib/ImportCache.c \
	$(E8025)/source/scriptlib/Instruction.c \
	$(E8025)/source/scriptlib/Interpreter.c \
	$(E8025)/source/scriptlib/Lexer.c \
	$(E8025)/source/scriptlib/List.c \
	$(E8025)/source/scriptlib/Parser.c \
	$(E8025)/source/scriptlib/ParserSet.c \
	$(E8025)/source/scriptlib/ScriptVariant.c \
	$(E8025)/source/scriptlib/ScriptVariantInteger.c \
	$(E8025)/source/scriptlib/Stack.c \
	$(E8025)/source/scriptlib/StackedSymbolTable.c \
	$(E8025)/source/scriptlib/SymbolTable.c \
	$(E8025)/source/stringptr.c \
	$(E8025)/source/stristr.c \
	$(E8025)/source/utils.c \
	$(E8025)/libretro/glue.c \
	$(E8025)/libretro/video.c \
	$(E8025)/libretro/control.c \
	$(E8025)/libretro/sblaster.c \
	$(E8025)/libretro/timer.c

# 8025_webm: the same engine with WebM video playback (webmlib + libvpx from
# deps/, decoded in threads), as upstream's WebM-enabled builds
ENGINE_8025_webm_NAME    := v4.0 Build 8025 + WebM
# + SAB: native script extensions (sab/README.md); only new sab_* builtins,
# nothing changes for existing mods
ENGINE_8025_webm_FLAGS   := $(ENGINE_8025_FLAGS) -DWEBM -DSAB
ENGINE_8025_webm_SOURCES := $(ENGINE_8025_SOURCES) \
	$(E8025)/source/webmlib/halloc/halloc.c \
	$(E8025)/source/webmlib/nestegg/nestegg.c \
	$(E8025)/source/webmlib/movie_playback.c \
	$(E8025)/source/webmlib/samplecvt.c \
	$(E8025)/source/webmlib/vidplay.c \
	$(E8025)/source/webmlib/vorbis.c \
	$(E8025)/source/webmlib/yuv.c \
	$(E8025)/libretro/threads.c
