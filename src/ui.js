/*
 * PitchGrid — Component UI for chain MIDI FX delegation
 *
 * Parameter menu (standard chain module pattern).
 * Knobs 1-6: adjust tuning parameters (with overlay feedback).
 * Back button: exits component UI → chain.
 * Jog wheel: navigates menu.
 *
 * Pad coloring is handled entirely by the C DSP module via firmware hooks.
 */

import {
    MidiCC,
    MoveShift, MoveMainKnob, MoveBack,
    MoveKnob1
} from '/data/UserData/move-anything/shared/constants.mjs';

import {
    decodeDelta, decodeAcceleratedDelta
} from '/data/UserData/move-anything/shared/input_filter.mjs';

import {
    drawMenuHeader, drawMenuList, drawMenuFooter,
    showOverlay, tickOverlay, drawOverlay
} from '/data/UserData/move-anything/shared/menu_layout.mjs';

import * as std from 'std';


/* ======================================================================
 * DEBUG LOGGING
 * ====================================================================== */

const LOG_PATH = '/data/UserData/move-anything/pitchgrid-debug.log';

function pgLog(msg) {
    try {
        const f = std.open(LOG_PATH, 'a');
        if (f) {
            f.puts(`[pg-ui] ${msg}\n`);
            f.close();
        }
    } catch (e) { /* ignore */ }
}


/* ======================================================================
 * MODULE STATE
 * ====================================================================== */

/* Tuning parameters — defaults to standard 5L2s diatonic */
let params = {
    a: 5, b: 2, mode: 0,
    equave: 1.0,
    generator: 7 / 12,
    steps: 7,
    offset: 5.5 / 12,
    baseFreq: 261.6256,
};

/* Knob parameter mapping */
const KNOB_PARAMS = [
    { key: 'generator', label: 'Generator', min: 0.01, max: 0.99, step: 0.002 },
    { key: 'equave',    label: 'Equave',    min: 0.5,  max: 2.0,  step: 0.005 },
    { key: 'steps',     label: 'Steps',     min: 2,    max: 31,   step: 1, integer: true },
    { key: 'offset',    label: 'Offset',    min: 0.0,  max: 1.0,  step: 0.005 },
    { key: 'mode',      label: 'Mode',      min: 0,    max: 20,   step: 1, integer: true },
    { key: 'baseFreq',  label: 'Base Freq', min: 100,  max: 880,  step: 1, integer: true },
];

/* Menu items */
const MENU_ITEMS = [
    { key: 'generator', label: 'Generator' },
    { key: 'equave',    label: 'Equave' },
    { key: 'steps',     label: 'Steps' },
    { key: 'offset',    label: 'Offset' },
    { key: 'mode',      label: 'Mode' },
    { key: 'baseFreq',  label: 'Base Freq' },
];

/* UI state */
let menuIndex = 0;
let shiftHeld = false;
let needsRedraw = true;


/* ======================================================================
 * DSP SYNC
 * ====================================================================== */

function syncParamsToDsp() {
    host_module_set_param("generator", String(params.generator));
    host_module_set_param("equave", String(params.equave));
    host_module_set_param("a", String(params.a));
    host_module_set_param("b", String(params.b));
    host_module_set_param("mode", String(params.mode));
    host_module_set_param("steps", String(params.steps));
    host_module_set_param("offset", String(params.offset));
    host_module_set_param("base_freq", String(params.baseFreq));
}

function syncParamsFromDsp() {
    const keys = [
        ['generator', parseFloat],
        ['equave', parseFloat],
        ['a', parseInt],
        ['b', parseInt],
        ['mode', parseInt],
        ['steps', parseInt],
        ['offset', parseFloat],
        ['base_freq', parseFloat],
    ];
    for (const [dspKey, parse] of keys) {
        const val = host_module_get_param(dspKey);
        if (val !== undefined && val !== null && val !== "") {
            const jsKey = dspKey === 'base_freq' ? 'baseFreq' : dspKey;
            params[jsKey] = parse(val);
        }
    }
}

function onParamChanged() {
    const maxMode = params.a + params.b - 1;
    if (params.mode > maxMode) params.mode = maxMode;
    syncParamsToDsp();
    needsRedraw = true;
}


/* ======================================================================
 * DISPLAY
 * ====================================================================== */

function getMenuValue(item) {
    const val = params[item.key];
    switch (item.key) {
        case 'generator': return `${(val * 1200).toFixed(0)}c`;
        case 'equave':    return val.toFixed(2);
        case 'steps':     return String(val);
        case 'offset':    return val.toFixed(3);
        case 'mode':      return String(val);
        case 'baseFreq':  return `${Math.round(val)}Hz`;
        default:          return String(val);
    }
}

function getOverlayValue(knobParam, val) {
    switch (knobParam.key) {
        case 'generator': return `${(val * 1200).toFixed(0)}c`;
        case 'equave':    return val.toFixed(3);
        case 'baseFreq':  return `${Math.round(val)}Hz`;
        default:          return knobParam.integer ? String(val) : val.toFixed(3);
    }
}

function drawMenu() {
    clear_screen();

    drawMenuHeader("PitchGrid", `${params.a}+${params.b} m${params.mode}`);

    drawMenuList({
        items: MENU_ITEMS,
        selectedIndex: menuIndex,
        getLabel: (item) => item.label,
        getValue: (item) => getMenuValue(item),
        valueAlignRight: true,
    });

    drawMenuFooter({ left: "Back: exit" });
    drawOverlay();
}


/* ======================================================================
 * MODULE LIFECYCLE (component UI interface)
 * ====================================================================== */

globalThis.init = function() {
    pgLog('=== PitchGrid component UI initializing ===');
    syncParamsFromDsp();
    menuIndex = 0;
    onParamChanged();
};

globalThis.tick = function() {
    if (tickOverlay()) needsRedraw = true;
    if (!needsRedraw) return;
    drawMenu();
    needsRedraw = false;
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    if (status !== MidiCC) return;

    /* Shift tracking */
    if (d1 === MoveShift) { shiftHeld = d2 === 127; return; }

    /* Back button: exit component UI → chain */
    if (d1 === MoveBack && d2 > 0) {
        host_return_to_menu();
        return;
    }

    /* Knobs 1-6: adjust tuning parameters */
    if (d1 >= MoveKnob1 && d1 <= MoveKnob1 + 5) {
        const knobIdx = d1 - MoveKnob1;
        const kp = KNOB_PARAMS[knobIdx];
        const delta = shiftHeld ? decodeDelta(d2) : decodeAcceleratedDelta(d2, d1);
        let val = params[kp.key] + delta * kp.step;
        val = Math.max(kp.min, Math.min(kp.max, val));
        if (kp.integer) val = Math.round(val);
        params[kp.key] = val;
        onParamChanged();
        showOverlay(kp.label, getOverlayValue(kp, val));
        return;
    }

    /* Jog wheel: navigate menu */
    if (d1 === MoveMainKnob) {
        const delta = decodeDelta(d2);
        if (delta !== 0) {
            menuIndex = Math.max(0, Math.min(MENU_ITEMS.length - 1, menuIndex + delta));
            needsRedraw = true;
        }
    }
};
