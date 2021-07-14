uint8_t slavesResponding[(maxSlaves + 1 + 7) / 8];
uint8_t  masks[8] = {1, 2, 4, 8, 16, 32, 64, 128};

typedef struct {
  byte tid[2];            // MBAP Transaction ID
  byte uid;               // MBAP Unit ID (address)
  byte PDUlen;            // lenght of PDU (func + data) stored in queuePDUs
  IPAddress remIP;        // remote IP for UDP client
  unsigned int remPort;   // remote port 
  int clientNum;         // TCP client socket number, UDP_REQUEST (0xFF)
} header;

CircularBuffer<header, reqQueueCount> queueHeaders;      
CircularBuffer<byte, reqQueueSize> queuePDUs;         
CircularBuffer<byte, reqQueueCount> queueRetries;

WiFiClient client;

void recvTcp()
{
  if(modbusServer.hasClient())
  {
    client = modbusServer.available();
    client.flush();
    Serial.println("TCP Connected");
  }
    
  if (client) {
    
    unsigned int packetSize = client.available();
    if(packetSize == 0)
    {
      return;
    }
    
    ethRxCount += packetSize;

    // Modbus TCP frame is 4 bytes longer than Modbus RTU frame
    byte tcpInBuffer[modbusSize + 7]; 
    // Modbus TCP/UDP frame: [0][1] transaction ID, [2][3] protocol ID, 
    // [4][5] length and [6] unit ID (address).....
    // Modbus RTU frame: [0] address.....
    client.read(tcpInBuffer, sizeof(tcpInBuffer));
    client.flush();
    
    byte errorCode = checkRequest(tcpInBuffer, packetSize);
    
    Serial.println();
    Serial.print("Check Request error code : ");
    Serial.println(errorCode, HEX);
    
    byte pduStart;
    if (localConfig.enableRtuOverTcp) {
      // In Modbus RTU, Function code is second byte (after address)
      pduStart = 1;   
     }else {
      // In Modbus TCP/UDP, Function code is 8th byte (after address)
      pduStart = 7;
     }            

    if (errorCode == 0) {
      // Store in request queue: 2 bytes MBAP Transaction ID (ignored in Modbus RTU over TCP);
      // MBAP Unit ID (address); PDUlen (func + data);remote IP; remote port;
      // TCP client Number (socket) - 0xFF for UDP
      queueHeaders.push(header {{tcpInBuffer[0], tcpInBuffer[1]}, tcpInBuffer[pduStart - 1],
                        (byte)(packetSize - pduStart), {}, 0, client.fd()});
      queueRetries.push(0);
      for (byte i = 0; i < packetSize - pduStart; i++) {
        queuePDUs.push(tcpInBuffer[i + pduStart]);
      }
    } else if (errorCode != 0xFF) {
      // send back message with error code
      if (!localConfig.enableRtuOverTcp) {
        client.write(tcpInBuffer, 5);
        client.write(0x03);
      }
      client.write(tcpInBuffer[pduStart - 1]);   // address
      client.write(tcpInBuffer[pduStart] + 0x80);       // function + 0x80
      client.write(errorCode);
      if (localConfig.enableRtuOverTcp) {
        crc = 0xFFFF;
        calculateCRC(tcpInBuffer[pduStart - 1]);
        calculateCRC(tcpInBuffer[pduStart] + 0x80);
        calculateCRC(errorCode);
        client.write(lowByte(crc));        // send CRC, low byte first
        client.write(highByte(crc));
      }
      ethTxCount += 5;
      if (!localConfig.enableRtuOverTcp) ethTxCount += 4;
    }
  }
}

void processRequests()
{
  // Insert scan request into queue
  if (scanCounter != 0 && queueHeaders.available() > 1 && queuePDUs.available() > 1) {
    // Store scan request in request queue
    queueHeaders.push(header {{0x00, 0x00}, scanCounter, sizeof(scanCommand), {}, 0, SCAN_REQUEST});
    // scan requests are only sent once, so set "queueRetries" to one attempt below limit
    queueRetries.push(localConfig.serialRetry - 1);    
    for (byte i = 0; i < sizeof(scanCommand); i++) {
      queuePDUs.push(scanCommand[i]);
    }
    scanCounter++;
    if (scanCounter == maxSlaves + 1) scanCounter = 0;
  }

  // Optimize queue (prioritize requests from responding slaves) and trigger sending via serial
  if (serialState == IDLE) {               // send new data over serial only if we are not waiting for response
    if (!queueHeaders.isEmpty()) {
      boolean queueHasRespondingSlaves;               // true if  queue holds at least one request to responding slaves
      for (byte i = 0; i < queueHeaders.size(); i++) {
        if (getSlaveResponding(queueHeaders[i].uid) == true) {
          queueHasRespondingSlaves = true;
          break;
        } else {
          queueHasRespondingSlaves = false;
        }
      }
      while (queueHasRespondingSlaves == true && getSlaveResponding(queueHeaders.first().uid) == false) {
        // move requests to non responding slaves to the tail of the queue
        for (byte i = 0; i < queueHeaders.first().PDUlen; i++) {
          queuePDUs.push(queuePDUs.shift());
        }
        queueRetries.push(queueRetries.shift());
        queueHeaders.push(queueHeaders.shift());
      }
      serialState = SENDING;                   // trigger sendSerial()
    }
  }
}

byte checkRequest(byte buffer[], unsigned int bufferSize) {
  byte address;
  int i;
  
  if (localConfig.enableRtuOverTcp) 
    address = buffer[0];
  else 
    address = buffer[6];

  if (localConfig.enableRtuOverTcp) {   // check CRC for Modbus RTU over TCP/UDP
    if (checkCRC(buffer, bufferSize) == false) {
      return 0xFF;                         // reject: do nothing and return no error code
    }
  } else {                  // check MBAP header structure for Modbus TCP/UDP
    if (buffer[2] != 0x00 || buffer[3] != 0x00 || buffer[4] != 0x00 || buffer[5] != bufferSize - 6) {
      return 0xFF;                         // reject: do nothing and return no error code
    }
  }
  // allow only one request to non responding slaves
  if (queueHeaders.isEmpty() == false && getSlaveResponding(address) == false) {
    // start searching from tail because requests to non-responsive
    // slaves are usually towards the tail of the queue                       
    for (byte j = queueHeaders.size(); j > 0 ; j--) {      
      // return modbus error 11 (Gateway Target Device Failed to Respond)
      // - usually means that target device (address) is not present                                                       
      if (queueHeaders[j - 1].uid == address) {
        return 0x0B;                   
      }
    }
  }
  
  // check if we have space in request queue
  // return modbus error 6 (Slave Device Busy) - try again later
  if (queueHeaders.available() < 1 || (localConfig.enableRtuOverTcp && queuePDUs.available() < bufferSize - 1) || 
     (!localConfig.enableRtuOverTcp && queuePDUs.available() < bufferSize - 7)) {
    return 0x06;                       
  }
  // al checkes passed OK, we can store the incoming data in request queue
  return 0;
}

void deleteRequest()        // delete request from queue
{
  for (byte i = 0; i < queueHeaders.first().PDUlen; i++) {
    queuePDUs.shift();
  }
  queueHeaders.shift();
  queueRetries.shift();
}


bool getSlaveResponding(const uint8_t index)
{
  if (index >= maxSlaves) return false;     // error
  return (slavesResponding[index / 8] & masks[index & 7]) > 0;
}


void setSlaveResponding(const uint8_t index, const bool value)
{
  if (index >= maxSlaves) return;     // error
  if (value == 0) slavesResponding[index / 8] &= ~masks[index & 7];
  else slavesResponding[index / 8] |= masks[index & 7];
}
