# DualBoy: Testing Instructions

Follow these steps to test the current build of the DualBoy split-screen GBA emulator.

## 1. Launching the App
Open your terminal and run:
```bash
cd "/home/coreyb/Coding Projects/Applications/DualBoy"
npm run tauri dev
```

## 2. Loading a Game
1. Click the **"Load ROM"** button.
2. Select a `.gba` file from your computer.
3. The game should appear on **both** screens simultaneously.

## 3. Control Mapping (Laptop Optimized)
Both players can play on the same keyboard using the following layout:

### Player 1 (Left Hand Cluster)
*   **D-Pad**: `W`, `A`, `S`, `D`
*   **A Button**: `K`
*   **B Button**: `J`
*   **L Trigger**: `H`
*   **R Trigger**: `L`
*   **Start**: `Enter`
*   **Select**: `Backspace`

### Player 2 (Right Hand / Arrows Cluster)
*   **D-Pad**: `Arrow Keys`
*   **A Button**: `M`
*   **B Button**: `N`
*   **L Trigger**: `V`
*   **R Trigger**: `B`
*   **Start**: `P`
*   **Select**: `O`

## 4. Verification Checklist
*   [ ] **Visuals**: Are both screens running at a smooth 60FPS?
*   [ ] **Sync**: Do both instances stay in perfect sync (e.g., when walking in circles)?
*   [ ] **Multiplayer**: Try to enter a "Multiplayer" or "Trade" menu in-game to verify the **Virtual Link Cable** architecture is working.
*   [ ] **Input**: Do both sets of keys respond without delay?

## 5. Providing Feedback
Please report any issues with:
1. **Performance**: Any stuttering or screen tearing?
2. **Sync**: Did the games ever disconnect or drift apart?
3. **Controls**: Do these new mappings feel comfortable on your laptop?
