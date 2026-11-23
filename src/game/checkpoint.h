#pragma once
// The last checkpoint kept on disk (checkpoint.cpp): the options' CONTINUE FROM LAST CHECKPOINT row
#include <string>

namespace checkpoint {

// any thread: what the row shows, the saved mission and checkpoint, or why the last ask did not go
std::string Label();
// any thread: loads the saved checkpoint; done on the game thread's next tick
void Continue();
// any thread: a checkpoint is kept on disk (the title menu shows its button only then)
bool HasSave();
// any thread: the title menu button's text, from checkpoint_label.txt ({c} the checkpoint, {m} the mission)
std::string ButtonText();

}  // namespace checkpoint
