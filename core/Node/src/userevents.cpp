#include "userevents.h"

UserEvents::UserEvents() : QObject(nullptr)
{

}

UserEvents &UserEvents::instance()
{
    static UserEvents events;
    return events;
}
