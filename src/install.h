#pragma once

/* In-OS installer.
 *   install              menu of disks that are not the live one
 *   install <id>         same, but skip the menu
 *   install <id> yes     skip the confirmation prompt (still refuses the live disk)
 * Formats the target as one bootable MBR FAT32 partition, copies the live
 * root (utils, Doom, kernel, Limine) onto it, and installs Limine BIOS. */
void cmd_install(int argc, char **argv);