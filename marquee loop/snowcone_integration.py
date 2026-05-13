"""
snowcone_integration.py

Single source of truth for snowcone configuration consumed by
yeti-build's stage_08_splash.py. Mirrors libreldr_entries.py so the two
projects feel consistent.
"""

from dataclasses import dataclass


@dataclass(frozen=True)
class SplashFiles:
    binary_src:       str  # built artifact in the cloned repo
    binary_dst:       str  # absolute path inside the target rootfs
    openrc_src:       str  # OpenRC init file in the cloned repo
    openrc_dst:       str  # absolute path inside the target rootfs


# Paths are relative to the root of the cloned snowcone repo.
FILES = SplashFiles(
    binary_src = "snowcone",
    binary_dst = "/usr/sbin/snowcone",
    openrc_src = "snowcone.openrc",
    openrc_dst = "/etc/init.d/snowcone",
)


# Runlevel the service should be registered under. 'boot' starts before
# the display manager (which lives in 'default') so the splash is
# already on screen when the compositor comes up.
RUNLEVEL = "boot"

# Kernel cmdline tokens that complement the splash. yeti-build's
# stage_07_bootloader doesn't currently template the cmdline — these
# are documented here so when you do start templating it, you know
# which flags pair with the splash.
RECOMMENDED_CMDLINE_TOKENS = [
    "quiet",           # silence kernel printk so the splash isn't overwritten
    "loglevel=3",      # belt-and-suspenders alongside `quiet`
    "vt.global_cursor_default=0",  # hide the text-mode cursor
]


def cmdline_addition() -> str:
    """The string yeti-build can append to options=... in libreldr_entries."""
    return " ".join(RECOMMENDED_CMDLINE_TOKENS)
