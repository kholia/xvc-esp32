
/*
 * Description :  Xilinx Virtual Cable Server for ESP32
 *
 * See Licensing information at End of File.
 */

#include <M5Atom.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <cstdlib>

static const char* MY_SSID = "ssid";
static const char* MY_PASSPHRASE = "wifi_passphrase";

#define ERROR_JTAG_INIT_FAILED -1
#define ERROR_OK 1

/* GPIO numbers for each signal. Negative values are invalid */
/* Note that currently only supports GPIOs below 32 to improve performance */
static constexpr const int tms_gpio = 22;
static constexpr const int tck_gpio = 19;
static constexpr const int tdo_gpio = 21;
static constexpr const int tdi_gpio = 25;

//static inline volatile std::uint32_t* gpio_set_reg(std::uint8_t pin) { return pin >= 32 ? &GPIO.out1_w1ts.val : &GPIO.out_w1ts; }
//static inline volatile std::uint32_t* gpio_clear_reg(std::uint8_t pin) { return pin >= 32 ? &GPIO.out1_w1tc.val : &GPIO.out_w1tc; }

#define GPIO_CLEAR  (GPIO.out_w1tc)
#define GPIO_SET    (GPIO.out_w1ts)
#define GPIO_IN     (GPIO.in)

/* Transition delay coefficients */
static const unsigned int jtag_delay = 10;

static std::uint32_t jtag_xfer(std::uint_fast8_t n, std::uint32_t tms, std::uint32_t tdi)
{
	std::uint32_t tdo = 0;
    const std::uint32_t tdo_bit = (1u << (n - 1));
	for (int i = 0; i < n; i++) {
		jtag_write(0, tms & 1, tdi & 1);
        asm volatile ("nop");
        asm volatile ("nop");
        asm volatile ("nop");
		jtag_write(1, tms & 1, tdi & 1);
    	for (std::uint32_t i = 0; i < jtag_delay; i++) asm volatile ("nop");
        tdo >>= 1;
        tdo |= jtag_read() ? tdo_bit : 0;
        jtag_write(0, tms & 1, tdi & 1);
    	for (std::uint32_t i = 0; i < jtag_delay; i++) asm volatile ("nop");
		tms >>= 1;
		tdi >>= 1;
        
	}
	return tdo;
}

static bool jtag_read(void)
{
	return !!(GPIO_IN & 1<<tdo_gpio);
}

static void jtag_write(std::uint_fast8_t tck, std::uint_fast8_t tms, std::uint_fast8_t tdi)
{
	const uint32_t set = tck<<tck_gpio | tms<<tms_gpio | tdi<<tdi_gpio;
	const uint32_t clear = !tck<<tck_gpio | !tms<<tms_gpio | !tdi<<tdi_gpio;

	GPIO_SET = set;
	GPIO_CLEAR = clear;
}

static int jtag_init(void)
{
	GPIO_CLEAR = 1<<tdi_gpio | 1<<tck_gpio;
	GPIO_SET = 1<<tms_gpio;

    pinMode(tdo_gpio, INPUT);
    pinMode(tdi_gpio, OUTPUT);
    pinMode(tck_gpio, OUTPUT);
    pinMode(tms_gpio, OUTPUT);

	jtag_write(0, 1, 0);

	return ERROR_OK;
}

static int sread(int fd, void* target, int len) {
   std::uint8_t *t = reinterpret_cast<std::uint8_t*>(target);
   while (len) {
      int r = read(fd, t, len);
      if (r <= 0)
         return r;
      t += r;
      len -= r;
   }
   return 1;
}

static constexpr const char* TAG = "XVC";

struct Socket
{
    int fd;
    Socket() : fd(-1) {}
    Socket(int fd) : fd(fd) {}
    Socket(int domain, int family, int protocol) 
    {
        this->fd = socket(domain, family, protocol);
    }
    Socket(const Socket&) = delete;
    Socket(Socket&& rhs) : fd(rhs.fd) { rhs.fd = -1; }
    ~Socket() { this->release(); }
    void release() { 
        if( this->is_valid() ) {
            closesocket(this->fd);
            this->fd = -1;
        }
    }
    int get() const { return this->fd;}

    Socket& operator=(Socket&& rhs) { this->fd = rhs.fd; rhs.fd = -1; return *this; }
    bool is_valid() const { return this->fd > 0; }
    operator int() const { return this->get();}
};

class XvcServer
{
private:
    Socket listen_socket;
    Socket client_socket;
    unsigned char buffer[2048], result[1024];
public:
    XvcServer(std::uint16_t port) {
        Socket sock(AF_INET, SOCK_STREAM, 0);
        {
            int value = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
        }

        sockaddr_in address;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);
        address.sin_family = AF_INET;

        if( bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ) {
            ESP_LOGE(TAG, "Failed to bind socket.");
        }
        if( listen(sock, 0) < 0 ) {
            ESP_LOGE(TAG, "Failed to listen the socket.");
        }

        ESP_LOGI(TAG, "Begin XVC Server. port=%d", port);
        this->listen_socket = std::move(sock);
    }
    XvcServer(const XvcServer&) = delete;

    bool wait_connection()
    {
        fd_set conn;
        int maxfd = this->listen_socket.get();

        FD_ZERO(&conn);
        FD_SET(this->listen_socket.get(), &conn);

        fd_set read = conn, except = conn;
        int fd;

        if (select(maxfd + 1, &read, 0, &except, 0) < 0) {
            ESP_LOGE(TAG, "select");
            return false;
        }
        
        for (fd = 0; fd <= maxfd; ++fd) {
            if (FD_ISSET(fd, &read)) {
                if (fd == this->listen_socket.get()) {
                    int newfd;
                    sockaddr_in address;
                    socklen_t nsize = sizeof(address);
                    newfd = accept(this->listen_socket.get(), reinterpret_cast<sockaddr*>(&address), &nsize);

                    ESP_LOGI(TAG, "connection accepted - fd %d\n", newfd);
                    if (newfd < 0) {
                        ESP_LOGE(TAG, "accept returned an error.");
                    } else {
                        if (newfd > maxfd) {
                            maxfd = newfd;
                        }
                        FD_SET(newfd, &conn);
                        this->client_socket = Socket(newfd);
                        return true;
                    }
                }
                
            }
        }

        return false;
    }

    bool handle_data()
    {
        const char xvcInfo[] = "xvcServer_v1.0:2048\n";
        int fd = this->client_socket.get();

        std::uint8_t cmd[16];
        std::memset(cmd, 0, 16);

        if (sread(fd, cmd, 2) != 1)
            return false;

        if (memcmp(cmd, "ge", 2) == 0) {
            if (sread(fd, cmd, 6) != 1)
                return 1;
            memcpy(result, xvcInfo, strlen(xvcInfo));
            if (write(fd, result, strlen(xvcInfo)) != strlen(xvcInfo)) {
                ESP_LOGE(TAG, "write");
                return 1;
            }
            ESP_LOGD(TAG, "%u : Received command: 'getinfo'\n", (int)time(NULL));
            ESP_LOGD(TAG, "\t Replied with %s\n", xvcInfo);
            return true;
        } else if (memcmp(cmd, "se", 2) == 0) {
            if (sread(fd, cmd, 9) != 1)
                return 1;
            memcpy(result, cmd + 5, 4);
            if (write(fd, result, 4) != 4) {
                ESP_LOGE(TAG, "write");
                return 1;
            }
            ESP_LOGD(TAG, "%u : Received command: 'settck'\n", (int)time(NULL));
            ESP_LOGD(TAG, "\t Replied with '%.*s'\n\n", 4, cmd + 5);
        
            return true;
        } else if (memcmp(cmd, "sh", 2) == 0) {
            if (sread(fd, cmd, 4) != 1)
                return false;
            ESP_LOGD(TAG, "%u : Received command: 'shift'\n", (int)time(NULL));
        } else {

            ESP_LOGE(TAG, "invalid cmd '%s'\n", cmd);
            return false;
        }

        int len;
        if (sread(fd, &len, 4) != 1) {
            ESP_LOGE(TAG, "reading length failed\n");
            return false;
        }

        int nr_bytes = (len + 7) / 8;
        if (nr_bytes * 2 > sizeof(buffer)) {
            ESP_LOGE(TAG, "buffer size exceeded\n");
            return false;
        }

        if (sread(fd, buffer, nr_bytes * 2) != 1) {
            ESP_LOGE(TAG, "reading data failed\n");
            return false;
        }
        memset(result, 0, nr_bytes);

        ESP_LOGD(TAG, "Number of Bits  : %d\n", len);
        ESP_LOGD(TAG, "Number of Bytes : %d \n", nr_bytes);
    
        jtag_write(0, 1, 1);

        int bytesLeft = nr_bytes;
        int bitsLeft = len;
        int byteIndex = 0;
        uint32_t tdi, tms, tdo;

        while (bytesLeft > 0) {
            tms = 0;
            tdi = 0;
            tdo = 0;
            if (bytesLeft >= 4) {
                memcpy(&tms, &buffer[byteIndex], 4);
                memcpy(&tdi, &buffer[byteIndex + nr_bytes], 4);

                tdo = jtag_xfer(32, tms, tdi);
                memcpy(&result[byteIndex], &tdo, 4);

                bytesLeft -= 4;
                bitsLeft -= 32;
                byteIndex += 4;

                ESP_LOGD(TAG, "LEN : 0x%08x\n", 32);
                ESP_LOGD(TAG, "TMS : 0x%08x\n", tms);
                ESP_LOGD(TAG, "TDI : 0x%08x\n", tdi);
                ESP_LOGD(TAG, "TDO : 0x%08x\n", tdo);
            } else {
                memcpy(&tms, &buffer[byteIndex], bytesLeft);
                memcpy(&tdi, &buffer[byteIndex + nr_bytes], bytesLeft);

                tdo = jtag_xfer(bitsLeft, tms, tdi);
                memcpy(&result[byteIndex], &tdo, bytesLeft);

                bytesLeft = 0;

                ESP_LOGD(TAG, "LEN : 0x%08x\n", bitsLeft);
                ESP_LOGD(TAG, "TMS : 0x%08x\n", tms);
                ESP_LOGD(TAG, "TDI : 0x%08x\n", tdi);
                ESP_LOGD(TAG, "TDO : 0x%08x\n", tdo);
                break;
            }
        }

        jtag_write(0, 1, 0);

        if (write(fd, result, nr_bytes) != nr_bytes) {
            ESP_LOGE(TAG, "write");
            return false;
        }

        return true;
    }

    void run()
    {
        if( this->client_socket.is_valid() ) {
            if( !this->handle_data() ) {
                this->client_socket.release();
            }
        }
        else {
            if( this->wait_connection() ) {
                // Nothing to do.
            }
        }
    }
};

static void serialTask(void*)
{
    std::size_t pendingBytesH2T = 0;
    std::size_t bytesWrittenH2T = 0;
    static std::uint8_t h2tBuffer[128];
    std::size_t pendingBytesT2H = 0;
    std::size_t bytesWrittenT2H = 0;
    static std::uint8_t t2hBuffer[128];
    
    while(true) {
        if( Serial.available() ) {
            Serial1.write(Serial.read());
        }
        else if( Serial1.available() ) {
            Serial.write(Serial1.read());
        }
        else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

void setup()
{
    jtag_init();

    WiFi.begin(MY_SSID, MY_PASSPHRASE);
    Serial.begin(115200);
    Serial1.begin(115200, SERIAL_8N1, 33, 23);
    
    TaskHandle_t handle;
    xTaskCreatePinnedToCore(serialTask,"serial", 4096, nullptr, 1, &handle, APP_CPU_NUM);
}

enum class AppState
{
    WaitingAPConnection,
    APConnected,
    ClientConnected,
};

static std::unique_ptr<XvcServer> server;

void loop()
{
    static AppState state = AppState::WaitingAPConnection;
    switch(state) {
    case AppState::WaitingAPConnection: {
        if( WiFi.isConnected() ) {
            ESP_LOGI(TAG, "WiFi connection ready. IP: %s", WiFi.localIP().toString().c_str());
            state = AppState::APConnected; 
        }
        break;
    }
    case AppState::APConnected: {
        if( !WiFi.isConnected() ) {
            ESP_LOGI(TAG, "disconnected from WiFi.");
            server.release();
            state = AppState::WaitingAPConnection;
            break;
        }

        if( !server ) {
            server.reset(new XvcServer(2542));
        }
        if( server ) {
            server->run();
        }
        break;
    }
    }
}


/*
 * This work, "xvc-esp32.ino", is a derivative of "xvcpi.c" (https://github.com/derekmulcahy/xvcpi)
 * by Derek Mulcahy.
 *
 * "xvc-esp32.ino" is licensed under CC0 1.0 Universal (http://creativecommons.org/publicdomain/zero/1.0/)
 * by Kenta IDA (fuga@fugafuga.org)
 *
 * The original license information of "xvcpi.c" is attached below.
 */

/*
 * This work, "xvcpi.c", is a derivative of "xvcServer.c" (https://github.com/Xilinx/XilinxVirtualCable)
 * by Avnet and is used by Xilinx for XAPP1251.
 *
 * "xvcServer.c" is licensed under CC0 1.0 Universal (http://creativecommons.org/publicdomain/zero/1.0/)
 * by Avnet and is used by Xilinx for XAPP1251.
 *
 * "xvcServer.c", is a derivative of "xvcd.c" (https://github.com/tmbinc/xvcd)
 * by tmbinc, used under CC0 1.0 Universal (http://creativecommons.org/publicdomain/zero/1.0/).
 *
 * Portions of "xvcpi.c" are derived from OpenOCD (http://openocd.org)
 *
 * "xvcpi.c" is licensed under CC0 1.0 Universal (http://creativecommons.org/publicdomain/zero/1.0/)
 * by Derek Mulcahy.*
 */