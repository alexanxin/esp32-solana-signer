#pragma once
// Dashboard mode's screens (CYD blend). Call after kitBegin() and the boot animation.
// Shows Home if Wi-Fi is set up, the setup network if not, or "Unlock the Signer once" if
// no address was saved yet. Never asks for the PIN.
void dashUiBegin();
void dashUiService();  // call often from loop()
