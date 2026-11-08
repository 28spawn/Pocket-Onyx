#!/usr/bin/env python3
"""
tools/update_device_ui.py - Strict circular geometry enforcement:
  - All buttons and knobs (PARA outer knob, PARA center push-button, MASTER dial, and bottom 5 buttons)
    are strictly round circles (aspect-ratio: 1/1, border-radius: 50%, flex-shrink: 0).
  - Isolates labels to absolute positioning so flexbox can NEVER squeeze buttons into ovals.
"""

import os

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
UI_HTML = os.path.join(FW, "emu", "ui.html")
WORKBENCH_HTML = os.path.join(FW, "wasm", "workbench.html")

STRICT_ROUND_CSS = """
  /* ========================================================================
     Pocket Amp Authentic Hardware Enclosure (Strict Circular Geometry)
     ======================================================================== */
  .device-shell {
    position: relative;
    background: linear-gradient(155deg, #262c38 0%, #171a22 45%, #0e1015 100%);
    border: 3.5px solid #3c4658;
    border-radius: 42px;
    padding: 24px 22px 22px;
    box-shadow: 
      0 22px 50px rgba(0,0,0,0.85),
      0 6px 16px rgba(0,0,0,0.6),
      inset 0 1.5px 2px rgba(255,255,255,0.2),
      inset 0 -2px 5px rgba(0,0,0,0.85);
    width: 580px;
    box-sizing: border-box;
    margin: 0 auto;
    user-select: none;
  }

  .bumper-accent {
    position: absolute;
    width: 32px;
    height: 32px;
    border-color: #55627a;
    border-style: solid;
    pointer-events: none;
  }
  .bumper-tl { top: 7px; left: 7px; border-width: 3.5px 0 0 3.5px; border-top-left-radius: 30px; }
  .bumper-tr { top: 7px; right: 7px; border-width: 3.5px 3.5px 0 0; border-top-right-radius: 30px; }
  .bumper-bl { bottom: 7px; left: 7px; border-width: 0 0 3.5px 3.5px; border-bottom-left-radius: 30px; }
  .bumper-br { bottom: 7px; right: 7px; border-width: 0 3.5px 3.5px 0; border-bottom-right-radius: 30px; }

  .hw-top-edge {
    position: absolute;
    top: -9px;
    left: 28px;
    right: 28px;
    height: 9px;
    display: flex;
    justify-content: flex-end;
    align-items: flex-end;
    padding-right: 68px;
    pointer-events: none;
  }
  .hw-power-switch {
    width: 42px;
    height: 8px;
    background: #2a313e;
    border: 1px solid #4a566d;
    border-bottom: none;
    border-radius: 4px 4px 0 0;
    box-shadow: inset 0 1px 2px rgba(255,255,255,0.25);
  }

  .device-faceplate {
    position: relative;
    background: #14171f;
    border: 2px solid #272e3d;
    border-radius: 28px;
    padding: 18px 20px 20px;
    box-shadow: inset 0 3px 14px rgba(0,0,0,0.75), 0 1px 2px rgba(255,255,255,0.06);
    display: flex;
    flex-direction: column;
    align-items: center;
  }

  .device-upper {
    display: flex;
    align-items: center;
    justify-content: space-between;
    width: 486px;
    height: 246px;
  }

  .lcd-bezel {
    background: #06080b;
    border: 3.5px solid #28303f;
    border-radius: 26px;
    width: 236px;
    height: 236px;
    box-sizing: border-box;
    display: flex;
    align-items: center;
    justify-content: center;
    box-shadow: 
      inset 0 4px 14px rgba(0,0,0,0.9),
      0 2px 6px rgba(0,0,0,0.5),
      0 0 1px rgba(255,255,255,0.1);
    position: relative;
    overflow: hidden;
    flex-shrink: 0;
  }
  .lcd-bezel::after {
    content: "";
    position: absolute;
    top: 0; left: 0; right: 0; height: 35%;
    background: linear-gradient(180deg, rgba(255,255,255,0.05) 0%, transparent 100%);
    border-top-left-radius: 22px;
    border-top-right-radius: 22px;
    pointer-events: none;
  }
  #lcdCanvas, canvas#lcd {
    width: 196px !important;
    height: 196px !important;
    image-rendering: pixelated;
    display: block;
    background: #000;
    border-radius: 8px;
    box-shadow: 0 0 10px rgba(0,0,0,0.8);
  }

  .knobs-pod-container {
    width: 210px;
    height: 236px;
    display: flex;
    align-items: center;
    justify-content: center;
    position: relative;
    flex-shrink: 0;
  }

  /* Tilted Pod Housing: 24.7° tilt from vertical */
  .tilted-pod {
    width: 98px;
    height: 236px;
    background: linear-gradient(145deg, #0d1016 0%, #171b23 100%);
    border: 2.5px solid #2b3444;
    border-radius: 49px;
    box-shadow: 
      inset 0 4px 14px rgba(0,0,0,0.95),
      inset 0 -2px 6px rgba(255,255,255,0.04),
      0 3px 10px rgba(0,0,0,0.65);
    transform: rotate(24.7deg);
    display: flex;
    flex-direction: column;
    justify-content: space-between;
    align-items: center;
    padding: 12px 0;
    box-sizing: border-box;
  }

  /* Knobs are strictly round (aspect-ratio: 1/1, border-radius: 50%, flex-shrink: 0) */
  .hw-knob-housing {
    position: relative;
    width: 68px;
    height: 68px;
    flex: 0 0 68px;
    flex-shrink: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    box-sizing: border-box;
  }

  .hw-rotary-knob {
    width: 68px !important;
    height: 68px !important;
    min-width: 68px !important;
    min-height: 68px !important;
    max-width: 68px !important;
    max-height: 68px !important;
    aspect-ratio: 1 / 1 !important;
    border-radius: 50% !important;
    background: radial-gradient(circle at 40% 35%, #465166 0%, #202633 55%, #13171f 100%);
    border: 3px solid #56647c;
    box-shadow: 
      0 6px 14px rgba(0,0,0,0.85),
      inset 0 2px 3px rgba(255,255,255,0.3),
      inset 0 -3px 5px rgba(0,0,0,0.8);
    position: relative;
    user-select: none;
    touch-action: none;
    box-sizing: border-box !important;
    flex-shrink: 0 !important;
    transition: border-color 0.15s ease;
  }
  .hw-rotary-knob::before {
    content: "";
    position: absolute;
    top: 5px; left: 5px; right: 5px; bottom: 5px;
    border-radius: 50%;
    border: 1px dashed rgba(255,255,255,0.18);
    pointer-events: none;
  }
  .hw-knob-dot {
    position: absolute;
    width: 8px !important;
    height: 8px !important;
    aspect-ratio: 1 / 1 !important;
    border-radius: 50% !important;
    background: #ffffff;
    box-shadow: 0 0 4px rgba(0,0,0,0.8), 0 0 2px #fff;
    top: 6px;
    left: 50%;
    transform: translateX(-50%);
    pointer-events: none;
    box-sizing: border-box;
  }
  .hw-rotary-knob.master-pot .hw-knob-dot {
    background: #ffd54f;
    box-shadow: 0 0 4px rgba(0,0,0,0.8), 0 0 3px #ffb300;
  }

  /* PARA Center Push-Button: Strict Round Circle Button */
  .hw-knob-center-click {
    position: absolute;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    width: 38px !important;
    height: 38px !important;
    min-width: 38px !important;
    min-height: 38px !important;
    max-width: 38px !important;
    max-height: 38px !important;
    aspect-ratio: 1 / 1 !important;
    border-radius: 50% !important;
    background: radial-gradient(circle at 45% 40%, #353f52 0%, #181d26 80%);
    border: 2px solid #4a5770;
    box-shadow: 
      0 3px 6px rgba(0,0,0,0.6),
      inset 0 1px 2px rgba(255,255,255,0.25);
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    outline: none;
    box-sizing: border-box !important;
    flex-shrink: 0 !important;
    z-index: 5;
    transition: transform 0.08s ease, border-color 0.1s ease, box-shadow 0.1s ease;
  }
  .hw-knob-center-click::after {
    content: "";
    width: 10px !important;
    height: 10px !important;
    aspect-ratio: 1 / 1 !important;
    border-radius: 50% !important;
    background: #00e5ff;
    box-shadow: 0 0 6px rgba(0,229,255,0.8);
    opacity: 0.85;
    transition: opacity 0.1s ease, box-shadow 0.1s ease;
    pointer-events: none;
  }
  .hw-knob-center-click:hover {
    border-color: #00e5ff;
  }
  .hw-knob-center-click:active, .hw-knob-center-click.pressed, .hw-knob-center-click.down {
    transform: translate(-50%, -50%) scale(0.92);
    box-shadow: inset 0 2px 5px rgba(0,0,0,0.9), 0 0 10px rgba(0,229,255,0.5);
    border-color: #00e5ff;
    background: #0e2e38;
  }
  .hw-knob-center-click:active::after, .hw-knob-center-click.pressed::after, .hw-knob-center-click.down::after {
    opacity: 1;
    box-shadow: 0 0 12px #00e5ff;
  }

  .hw-rotary-knob.master-pot {
    border-color: #7a6640;
    cursor: grab;
  }
  .hw-rotary-knob.master-pot:active {
    border-color: #ffb300;
    cursor: grabbing;
  }

  /* Labels are absolutely positioned so they never squeeze or distort the round knobs */
  .hw-knob-label {
    position: absolute;
    bottom: -16px;
    left: 50%;
    transform: translateX(-50%) rotate(-24.7deg);
    font-size: 10px;
    font-weight: 800;
    letter-spacing: 1.2px;
    color: #8f9bb3;
    pointer-events: none;
    white-space: nowrap;
    text-shadow: 0 1px 3px rgba(0,0,0,0.9);
  }

  /* Bottom Horizontal Pill-Shaped Bar containing 5 STRICTLY ROUND tactile buttons */
  .hw-pill-bar {
    display: flex;
    align-items: center;
    justify-content: space-around;
    background: #0b0e14;
    border: 2.5px solid #2a3344;
    border-radius: 9999px;
    padding: 6px 14px;
    box-shadow: 
      inset 0 3px 10px rgba(0,0,0,0.95),
      0 2px 5px rgba(0,0,0,0.5),
      0 1px 1px rgba(255,255,255,0.06);
    width: 486px;
    height: 66px;
    margin-top: 18px;
    box-sizing: border-box;
  }

  /* Every bottom button is a tactile, strictly round circular push-button */
  .hw-pill-btn {
    width: 50px !important;
    height: 50px !important;
    min-width: 50px !important;
    min-height: 50px !important;
    max-width: 50px !important;
    max-height: 50px !important;
    aspect-ratio: 1 / 1 !important;
    border-radius: 50% !important;
    background: radial-gradient(circle at 45% 35%, #2c3545 0%, #171d28 60%, #0f131a 100%);
    border: 2px solid #3d4a60;
    color: #f1f5f9;
    font-family: inherit;
    font-size: 11.5px;
    font-weight: 800;
    letter-spacing: 0.5px;
    text-align: center;
    cursor: pointer;
    user-select: none;
    touch-action: none;
    transition: all 0.08s ease;
    position: relative;
    outline: none;
    display: flex;
    align-items: center;
    justify-content: center;
    box-shadow: 
      0 3px 6px rgba(0,0,0,0.6),
      inset 0 1px 1.5px rgba(255,255,255,0.22);
    flex-shrink: 0 !important;
    box-sizing: border-box !important;
  }
  .hw-pill-btn:hover {
    border-color: var(--accent);
    color: var(--accent);
    box-shadow: 0 0 8px rgba(0,229,255,0.3);
  }
  .hw-pill-btn:active, .hw-pill-btn.down {
    transform: translateY(2px) scale(0.94);
    background: radial-gradient(circle at 45% 45%, #0e303a 0%, #06171d 100%);
    border-color: var(--accent);
    color: var(--accent);
    box-shadow: inset 0 2px 4px rgba(0,0,0,0.9), 0 0 12px rgba(0,229,255,0.4);
  }
  .hw-pill-divider {
    width: 2.5px;
    height: 30px;
    background: #1c222e;
    border-radius: 2px;
    box-shadow: inset 0 1px 2px rgba(0,0,0,0.9), 1px 0 0 rgba(255,255,255,0.07);
    flex-shrink: 0;
  }
"""

def update_ui_html():
    print("[*] Enforcing strictly round buttons and knobs in emu/ui.html...")
    with open(UI_HTML, "r", encoding="utf-8") as f:
        content = f.read()

    css_start = content.find("Pocket Amp Authentic Hardware Enclosure")
    if css_start != -1:
        css_start = content.rfind("/*", 0, css_start)
    else:
        css_start = content.find("/* Pocket Amp Physical Hardware Enclosure */")
    css_end = content.find("/* Audio & ASIO Controls */")
    if css_start != -1 and css_end != -1:
        content = content[:css_start] + STRICT_ROUND_CSS.strip() + "\n\n  " + content[css_end:]
    else:
        print("[-] CSS markers not found in ui.html")
        return False

    with open(UI_HTML, "w", encoding="utf-8") as f:
        f.write(content)
    print("[+] Successfully updated emu/ui.html with strictly round buttons.")
    return True

def update_workbench_html():
    print("[*] Enforcing strictly round buttons and knobs in wasm/workbench.html...")
    with open(WORKBENCH_HTML, "r", encoding="utf-8") as f:
        content = f.read()

    css_start = content.find("Pocket Amp Authentic Hardware Enclosure")
    if css_start != -1:
        css_start = content.rfind("/*", 0, css_start)
    else:
        css_start = content.find(".dev{background:#000")
    css_end = content.find("table{width:100%;border-collapse:collapse;font-size:12.5px}")
    if css_start != -1 and css_end != -1:
        content = content[:css_start] + STRICT_ROUND_CSS.strip() + "\n\n  " + content[css_end:]
    else:
        print("[-] CSS markers not found in workbench.html")
        return False

    with open(WORKBENCH_HTML, "w", encoding="utf-8") as f:
        f.write(content)
    print("[+] Successfully updated wasm/workbench.html with strictly round buttons.")
    return True

if __name__ == "__main__":
    update_ui_html()
    update_workbench_html()

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
