#ifndef MODULESTATEMACHINE_H
#define MODULESTATEMACHINE_H

#include <QObject>
#include <QHash>
#include <QString>

class ModuleStateMachine : public QObject
{
    Q_OBJECT

public:
    enum Module {
        Camera = 0,
        Ai,
        Rtmp,
        Sensor,
        Alarm
    };
    Q_ENUM(Module)

    enum State {
        Unknown = 0,
        Starting,
        Running,
        Warning,
        Error,
        Stopped
    };
    Q_ENUM(State)

    explicit ModuleStateMachine(QObject *parent = nullptr);

    void transition(Module module, State state, const QString &message = QString());
    State state(Module module) const;
    QString message(Module module) const;

signals:
    void moduleStateChanged(ModuleStateMachine::Module module,
                            ModuleStateMachine::State state,
                            const QString &message);

private:
    struct ModuleSnapshot
    {
        State state = Unknown;
        QString message;
    };

    QHash<int, ModuleSnapshot> m_modules;
};

#endif // MODULESTATEMACHINE_H
