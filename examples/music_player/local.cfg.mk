ENJ_BASENAME := aicaflow_music_player
ENJ_INJECT_QFONT := 1
OPTLEVEL := 2
ENJ_CFLAGS += -Wall -Wextra -Werror
ENJ_INCLUDES += -Ibuild/include -I../../format/include -I../../driver/include -I../../driver/sh4/include
ENJ_LDLIBS += ../../driver/sh4/libaicaflow_host.a
