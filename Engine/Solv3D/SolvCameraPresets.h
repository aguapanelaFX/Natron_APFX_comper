/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of SOLVFX (Natron fork) by AGUAPANELA FX.
 * GNU General Public License v2 or later.
 * ***** END LICENSE BLOCK ***** */

// Film back presets (sensor area used by each recording mode, in mm) for the Camera and VanishingCamera nodes.
// Phones: lenses are given as 35 mm-equivalent focal lengths, so the film back is the 35 mm-equivalent sensor
// (diagonal 43.27 mm) with the phone's aspect: the field of view is right without knowing the real sensor.
// Phone video is cropped a little by stabilisation: use the approximate focal mode.
// Values from the manufacturers' format sheets; entries marked (approx.) are rounded or estimated.

#ifndef SOLVFX_SOLVCAMERAPRESETS_H
#define SOLVFX_SOLVCAMERAPRESETS_H

namespace Solv3D {

struct CameraPreset
{
    const char* name;  // "Group/Camera mode" (the slash makes a sub-menu)
    double width, height; // film back (mm)
    double focal;      // typical / equivalent focal (mm), 0 = keep the current one
};

static const CameraPreset kCameraPresets[] = {
    { "custom", 0., 0., 0. },
    // ARRI
    { "ARRI/ALEXA 35 4.6K 3:2 Open Gate", 27.99, 19.22, 0. },
    { "ARRI/ALEXA 35 4.6K 16:9", 27.99, 15.75, 0. },
    { "ARRI/ALEXA 35 4K 16:9", 24.88, 14.00, 0. },
    { "ARRI/ALEXA 35 4K 2:1", 24.88, 12.44, 0. },
    { "ARRI/ALEXA 35 3.3K 6:5", 20.22, 16.95, 0. },
    { "ARRI/ALEXA 35 3K 1:1", 18.66, 18.66, 0. },
    { "ARRI/ALEXA 35 2K 16:9 S16", 12.44, 7.00, 0. },
    { "ARRI/ALEXA Mini 3.4K Open Gate", 28.25, 18.17, 0. },
    { "ARRI/ALEXA Mini 3.2K / 4K UHD", 26.40, 14.85, 0. },
    { "ARRI/ALEXA Mini 2.8K 4:3", 23.76, 17.82, 0. },
    { "ARRI/ALEXA Mini 2.8K 16:9 / HD / 2K", 23.76, 13.37, 0. },
    { "ARRI/ALEXA Mini LF 4.5K LF Open Gate", 36.70, 25.54, 0. },
    { "ARRI/ALEXA Mini LF 4.5K LF 2.39:1", 36.70, 15.31, 0. },
    { "ARRI/ALEXA Mini LF 3.8K LF 16:9 UHD", 31.68, 17.82, 0. },
    { "ARRI/ALEXA Mini LF 2.8K S35 16:9", 23.76, 13.37, 0. },
    { "ARRI/ALEXA LF 4.5K LF Open Gate", 36.70, 25.54, 0. },
    { "ARRI/ALEXA LF 4.5K LF 2.39:1", 36.70, 15.31, 0. },
    { "ARRI/ALEXA LF 3.8K LF 16:9 UHD", 31.68, 17.82, 0. },
    // RED (crop = resolution x photosite pitch)
    { "RED/V-RAPTOR 8K VV 8K 17:9", 40.96, 21.60, 0. },
    { "RED/V-RAPTOR 8K VV 7K 17:9", 35.84, 18.90, 0. },
    { "RED/V-RAPTOR 8K VV 6K 17:9", 30.72, 16.20, 0. },
    { "RED/V-RAPTOR 8K VV 5K 17:9", 25.60, 13.50, 0. },
    { "RED/V-RAPTOR 8K VV 4K 17:9", 20.48, 10.80, 0. },
    { "RED/V-RAPTOR 8K S35 8K 17:9", 26.21, 13.82, 0. },
    { "RED/V-RAPTOR 8K S35 6K 17:9", 19.66, 10.37, 0. },
    { "RED/V-RAPTOR 8K S35 4K 17:9", 13.11, 6.91, 0. },
    { "RED/KOMODO 6K 6K 17:9", 27.03, 14.26, 0. },
    { "RED/KOMODO 6K 5K 17:9", 22.53, 11.88, 0. },
    { "RED/KOMODO 6K 4K 17:9", 18.02, 9.50, 0. },
    // Sony
    { "Sony/VENICE 2 8.6K 3:2 Full Frame", 35.90, 24.00, 0. },
    { "Sony/VENICE 2 8.2K 17:9", 34.10, 18.00, 0. },
    { "Sony/VENICE 2 5.8K 17:9 S35 (approx.)", 24.30, 12.80, 0. },
    { "Sony/Alpha 7 V Full Frame (photo)", 35.90, 23.90, 0. },
    { "Sony/Alpha 7 V 4K 16:9 full width", 35.90, 20.19, 0. },
    { "Sony/Alpha 7 V 4K 16:9 APS-C / S35 crop (approx.)", 23.50, 13.22, 0. },
    { "Sony/Alpha 7R VI Full Frame (photo, approx.)", 35.70, 23.80, 0. },
    { "Sony/Alpha 7R VI 4K 16:9 full width (approx.)", 35.70, 20.08, 0. },
    { "Sony/Alpha 7S III Full Frame (photo)", 35.60, 23.80, 0. },
    { "Sony/Alpha 7S III 4K 16:9 full width", 35.60, 20.03, 0. },
    { "Sony/Alpha 7S III 4K 120p 1.1x crop", 32.36, 18.21, 0. },
    { "Sony/FX3 4K 16:9 full width", 35.60, 20.03, 0. },
    { "Sony/FX3 4K 120p 1.1x crop", 32.36, 18.21, 0. },
    { "Sony/FX5 4K 16:9 full width (approx.)", 35.60, 20.03, 0. },
    // Blackmagic
    { "Blackmagic/URSA Cine 12K LF 3:2", 36.00, 24.00, 0. },
    { "Blackmagic/PYXIS 6K Full Frame 3:2", 36.00, 24.00, 0. },
    { "Blackmagic/Pocket 6K (S35) 16:9", 23.10, 12.99, 0. },
    { "Blackmagic/Pocket 4K (Micro 4/3) 16:9", 18.96, 10.00, 0. },
    // stills / film standards
    { "Stills/Full Frame 3:2", 36.00, 24.00, 0. },
    { "Stills/Full Frame 16:9 video", 36.00, 20.25, 0. },
    { "Stills/APS-C 3:2 (Sony, Nikon, Fuji)", 23.50, 15.60, 0. },
    { "Stills/APS-C 3:2 (Canon)", 22.30, 14.90, 0. },
    { "Stills/Micro 4/3 4:3", 17.30, 13.00, 0. },
    { "Stills/Super 35 film 4-perf", 24.89, 18.66, 0. },
    { "Stills/35 mm Academy", 21.95, 16.00, 0. },
    // phones (35 mm-equivalent film back, photo 4:3 / video 16:9)
    { "Phones/iPhone 17 Pro main 24 mm (photo 4:3)", 34.62, 25.96, 24. },
    { "Phones/iPhone 17 Pro main 24 mm (video 16:9)", 34.62, 19.47, 24. },
    { "Phones/iPhone 17 Pro ultra wide 13 mm (video 16:9)", 34.62, 19.47, 13. },
    { "Phones/iPhone 17 Pro tele 4x 100 mm (video 16:9)", 34.62, 19.47, 100. },
    { "Phones/iPhone 16 Pro main 24 mm (video 16:9)", 34.62, 19.47, 24. },
    { "Phones/iPhone 16 Pro ultra wide 13 mm (video 16:9)", 34.62, 19.47, 13. },
    { "Phones/iPhone 16 Pro tele 5x 120 mm (video 16:9)", 34.62, 19.47, 120. },
    { "Phones/iPhone 17 / 16 main 26 mm (video 16:9)", 34.62, 19.47, 26. },
    { "Phones/iPhone 17 / 16 ultra wide 13 mm (video 16:9)", 34.62, 19.47, 13. },
    { "Phones/Galaxy S25 Ultra main 23 mm (video 16:9)", 34.62, 19.47, 23. },
    { "Phones/Galaxy S25 Ultra ultra wide 13 mm (video 16:9)", 34.62, 19.47, 13. },
    { "Phones/Galaxy S25 Ultra tele 3x 67 mm (video 16:9)", 34.62, 19.47, 67. },
    { "Phones/Galaxy S25 Ultra tele 5x 111 mm (video 16:9)", 34.62, 19.47, 111. },
    { "Phones/Pixel 10 Pro main 25 mm (video 16:9, approx.)", 34.62, 19.47, 25. },
    { "Phones/Pixel 10 Pro tele 5x 113 mm (video 16:9, approx.)", 34.62, 19.47, 113. },
    { "Phones/generic phone main 26 mm (photo 4:3)", 34.62, 25.96, 26. },
    { "Phones/generic phone main 26 mm (video 16:9)", 34.62, 19.47, 26. },
};

static const int kCameraPresetCount = (int)( sizeof(kCameraPresets) / sizeof(kCameraPresets[0]) );

} // namespace Solv3D

#endif // SOLVFX_SOLVCAMERAPRESETS_H
