OptiScaler plugins folder
=========================

OptiScaler loads every *.asi file in this folder at start ([Plugins] LoadAsiPlugins=true in OptiScaler.ini).

Optional: 3x / 4x frame generation on RTX 40 cards
--------------------------------------------------

DLSS Frame Generation only offers 2x on RTX 40 cards. The MFG Unlocker plugin by dashdogy raises that:

  1. Download it from https://github.com/dashdogy/RTX40MFG-Unlock/releases (V1.5.0 or newer).
  2. Rename MFGUnlocker.dll to MFGUnlocker.asi and put it in this folder.
  3. In game, open the OptiScaler menu (Insert) and set "Frame Gen" in the Frame Generation section to 3X or 4X.

Use only the plugin file. Do not install the OptiScaler build that is released next to it: it does not have the
changes this mod needs for Assassin's Creed Origins.
