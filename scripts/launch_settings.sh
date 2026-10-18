# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced by run.sh: bbport.ini settings the launcher writes that become environment variables.
#   fps_cap=30|40|60|90|120  the presenter lets at most that many frames through
#                            (BB_FPS_LIMIT); the game keeps run.sh's default frame rate patch
#   fullscreen=1             the window opens full screen (BB_FULLSCREEN=1)
#   metal_hud=1              Apple's Metal Performance HUD (MTL_HUD_ENABLED=1)
# Values already in the environment win. Bash 3.2 (macOS /bin/bash) compatible.
bb_launch_settings() { # ini file
    local line cap='' full='' hud=''
    [[ -f $1 ]] || return 0
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        if [[ $line =~ ^fps_cap=(30|40|60|90|120)$ ]]; then cap=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^fullscreen=([01])$ ]]; then full=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^metal_hud=([01])$ ]]; then hud=${BASH_REMATCH[1]}; fi
    done < "$1"
    if [[ -n $cap && -z ${BB_FPS_LIMIT:-} ]]; then export BB_FPS_LIMIT=$cap; fi
    if [[ $full == 1 && -z ${BB_FULLSCREEN:-} ]]; then export BB_FULLSCREEN=1; fi
    if [[ $hud == 1 && -z ${MTL_HUD_ENABLED:-} ]]; then export MTL_HUD_ENABLED=1; fi
    return 0
}
