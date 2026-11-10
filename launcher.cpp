#include "app.h"

void Launcher::add(App &app)
{
    if (count_ < kMaxApps)
    {
        apps_[count_++] = &app;
    }
}

App &Launcher::current()
{
    return *apps_[currentIndex_];
}
