#include "ModuleStateMachine.h"

ModuleStateMachine::ModuleStateMachine(QObject *parent)
    : QObject(parent)
{
    transition(Camera, Unknown, QStringLiteral("未检测"));
    transition(Ai, Unknown, QStringLiteral("未启动"));
    transition(Rtmp, Unknown, QStringLiteral("未连接"));
    transition(Sensor, Unknown, QStringLiteral("未连接"));
    transition(Alarm, Running, QStringLiteral("无"));
}

void ModuleStateMachine::transition(Module module, State state, const QString &message)
{
    ModuleSnapshot &snapshot = m_modules[static_cast<int>(module)];

    if(snapshot.state == state && snapshot.message == message)
        return;

    snapshot.state = state;
    snapshot.message = message;
    emit moduleStateChanged(module, state, message);
}

ModuleStateMachine::State ModuleStateMachine::state(Module module) const
{
    return m_modules.value(static_cast<int>(module)).state;
}

QString ModuleStateMachine::message(Module module) const
{
    return m_modules.value(static_cast<int>(module)).message;
}
