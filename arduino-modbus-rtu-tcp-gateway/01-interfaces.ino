void startSerial() {
#ifdef DEBUG
	Serial.begin(115200);
#endif
	mySerial.begin(9600, SERIAL_8N1);
	// Calculate Modbus RTU character timeout and frame delay
	byte bits =                                         // number of bits per character (11 in default Modbus RTU settings)
    1 +                                               // start bit
		(((localConfig.serialConfig & 0x06) >> 1) + 5) +  // data bits
		(((localConfig.serialConfig & 0x08) >> 3) + 1);   // stop bits
	if (((localConfig.serialConfig & 0x30) >> 4) > 1) bits += 1;    // parity bit (if present)
	int T = ((unsigned long)bits * 1000000UL) / localConfig.baud;       // time to send 1 character over serial in microseconds
	if (localConfig.baud <= 19200) {
		charTimeout = 1.5 * T;         // inter-character time-out should be 1,5T
		frameDelay = 3.5 * T;         // inter-frame delay should be 3,5T
	}
	else {
		charTimeout = 750;
		frameDelay = 1750;
	}
}

void WiFiDisconnected(WiFiEvent_t event, WiFiEventInfo_t info){
#ifdef DEBUG
    Serial.print("WiFi lost connection. Reason: ");
    Serial.println(info.disconnected.reason);
#endif
}

void startWifi() {
    WiFi.disconnect(true);
    delay(1000);
    WiFi.onEvent(WiFiDisconnected, WiFiEvent_t::SYSTEM_EVENT_STA_DISCONNECTED);
    WiFi.begin(ssid, password);
#ifdef DEBUG
    Serial.println();
    Serial.print(F("Connecting to "));
    Serial.println(F(ssid));
#endif
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
#ifdef DEBUG
        Serial.print(F("."));
#endif
    }
#ifdef DEBUG
    Serial.println(F(""));
    Serial.println(F("WiFi connected"));
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
	  Serial.print(F("Subnet mask: "));
    Serial.println(WiFi.subnetMask());
    Serial.print(F("Gateway IP: "));
    Serial.println(WiFi.gatewayIP()); 
#endif
	localConfig.ip = WiFi.localIP();
	localConfig.subnet = WiFi.subnetMask();
	localConfig.gateway = WiFi.gatewayIP();
	WiFi.macAddress(localConfig.mac);
	
	modbusServer.begin();
	webServer.begin();

	// Maximum connections
	maxSockNum = 4;

	dbg(F("[arduino] Server available at http://"));
}

void CheckWiFiConn()
{

}

void maintainUptime()
{
  unsigned long milliseconds = millis();
  if (last_milliseconds > milliseconds) {
    //in case of millis() overflow, store existing passed seconds
    remaining_seconds = seconds;
  }
  //store last millis(), so that we can detect on the next call
  //if there is a millis() overflow ( millis() returns 0 )
  last_milliseconds = milliseconds;
  //In case of overflow, the "remaining_seconds" variable contains seconds counted before the overflow.
  //We add the "remaining_seconds", so that we can continue measuring the time passed from the last boot of the device.
  seconds = (milliseconds / 1000) + remaining_seconds;
}

void maintainCounters()
{
	const unsigned long rollover = 0xFFFFFF00;
	
	if (serialTxCount > rollover || 
	    serialRxCount > rollover || 
	    ethTxCount > rollover ||
		ethRxCount > rollover) {
		serialRxCount = 0;
		serialTxCount = 0;
		ethRxCount = 0;
		ethTxCount = 0;
  }
}

void generateMac()
{
}

void CreateTrulyRandomSeed()
{
}
