#include <MQTTClient.h>

int main(void) {
    MQTTClient client = 0;
    return MQTTClient_create(&client, "tcp://127.0.0.1:1883", "probe",
                             MQTTCLIENT_PERSISTENCE_NONE, 0);
}
