/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// From SC
void AddPlayerbotsScripts();

// Add all
// Cortex's body (ported from mod-playerbots, see src/Body/README.md); called from Addmod_cortexScripts.
void AddCortexBodyScripts() { AddPlayerbotsScripts(); }
