#include <modbus/modbus.h>

int main(void) {
    modbus_t *ctx = modbus_new_rtu("/dev/null", 115200, 'N', 8, 1);
    modbus_free(ctx);
    return 0;
}
