Frame Generation for Assassin's Creed Origins
=============================================

Adds NVIDIA DLSS Frame Generation to Assassin's Creed Origins (DirectX 11). It is a build of OptiScaler with
changes for this game:

  * ReShade's effects and its menu stay visible and usable with frame generation on.
  * HDR works: the game's HDR picture is converted to HDR10, which DLSS Frame Generation needs.
  * Frame generation gets the game's real camera (field of view, movement, camera cuts) instead of guesses,
    and the scene's depth in the form it needs (the DLSS mod passes distances in metres). That keeps the
    area around Bayek clean when the camera turns fast, also at 3x.

Frame generation takes its input from DLSS upscaling, so the DLSS mod below is required.


WHAT YOU NEED
-------------

  * An NVIDIA RTX 40 or RTX 50 graphics card and a current driver.
  * Windows "Hardware-accelerated GPU scheduling" turned on (Settings > System > Display > Graphics >
    Default graphics settings). DLSS Frame Generation does not run without it.
  * "DLSS for Assassin's Creed Origins" (Nexus Mods, mod 339), installed and working. It brings ReShade
    (dxgi.dll) with it. If you installed ReShade yourself, use 6.8.0 or newer with full add-on support.


INSTALLING
----------

  1. Install and test the DLSS mod first: DLSS should be active in its panel (HOME key).
  2. Copy everything in this archive next to ACOrigins.exe:

         winmm.dll
         OptiScaler.ini
         OptiScaler\  (folder)

  3. Run OptiScaler\Streamlined_fetcher_windows.bat and press 1. It downloads NVIDIA Streamline, including
     DLSS Frame Generation (nvngx_dlssg.dll), from NVIDIA's GitHub into OptiScaler\streamline. Those files
     are NVIDIA's and can't be included here.
  4. Start the game. Frame generation is on as soon as DLSS is running.

Optional mods that work alongside: RenoDX, Origins GFX, and the MFG Unlocker plugin for 3x/4x on RTX 40
cards (see OptiScaler\plugins\README.txt).


USING IT
--------

  * The OptiScaler menu opens with INSERT. The "Frame Gen" slider in the Frame Generation section turns
    frame generation off or sets the multiplier. The ReShade menu stays on HOME.
  * The game only offers HDR in exclusive fullscreen. This mod turns exclusive fullscreen into borderless
    fullscreen behind the game's back, so HDR and frame generation work together: choose fullscreen in the
    game's options.
  * HDR: turn on HDR in Windows, then in the game's options as usual.
  * Turn off NVIDIA Smooth Motion for this game. It is the driver's own frame generation and does not
    combine with DLSS Frame Generation.
  * Frame generation adds a little input latency and works best from about 50-60 fps before frame
    generation. The game's HUD has no separate layer for frame generation, so moving HUD elements can
    show slight ghosting.
  * To judge the generated frames, tick "Show only generated frames" in the OptiScaler menu's DLSSG
    settings for a moment (motion looks wrong while it is on).


UNINSTALLING
------------

Delete winmm.dll, OptiScaler.ini and the OptiScaler folder (and OptiScaler.log if there is one). The DLSS
mod keeps working on its own.


REPORTING A PROBLEM
-------------------

Open OptiScaler.ini in Notepad, find the [Log] section and set

    LogToFile=true
    LogLevel=2

then play until the problem happens and attach OptiScaler.log from the game folder. Set LogToFile back to
auto afterwards.


CREDITS AND LICENCES
--------------------

  * OptiScaler by the OptiScaler team - GPL-3.0. winmm.dll is built from
    https://github.com/adnan-altaee/OptiScaler (see SOURCE.txt for the exact commit).
  * ReShade add-on API headers by crosire - BSD-3-Clause.
  * DirectX 12 Agility SDK runtime (OptiScaler\D3D12_OptiScaler) by Microsoft - see Licenses.
  * NVIDIA DLSS Frame Generation and Streamline are NVIDIA's and are downloaded from NVIDIA directly.
