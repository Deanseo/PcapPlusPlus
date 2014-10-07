#define LOG_MODULE PacketLogModulePacket

#include <Packet.h>
#include <EthLayer.h>
#include <Logger.h>
#include <string.h>
#include <typeinfo>

Packet::Packet(size_t maxPacketLen) :
	m_RawPacket(NULL),
	m_FirstLayer(NULL),
	m_LastLayer(NULL),
	m_ProtocolTypes(Unknown),
	m_MaxPacketLen(maxPacketLen),
	m_FreeRawPacket(true)
{
	timeval time;
	gettimeofday(&time, NULL);
	uint8_t* data = new uint8_t[m_MaxPacketLen];
	memset(data, 0, m_MaxPacketLen);
	m_RawPacket = new RawPacket((const uint8_t*)data, 0, time, true);
}

Packet::Packet(RawPacket* rawPacket) :
	m_FirstLayer(NULL),
	m_LastLayer(NULL),
	m_ProtocolTypes(Unknown),
	m_MaxPacketLen(rawPacket->getRawDataLen()),
	m_FreeRawPacket(false)
{
	m_RawPacket = rawPacket;
	m_FirstLayer = new EthLayer((uint8_t*)m_RawPacket->getRawData(), m_RawPacket->getRawDataLen());
	m_LastLayer = m_FirstLayer;
	Layer* curLayer = m_FirstLayer;
	while (curLayer != NULL)
	{
		m_ProtocolTypes |= curLayer->getProtocol();
		curLayer->parseNextLayer();
		m_LayersInitialized.push_back(curLayer);
		curLayer = curLayer->getNextLayer();
		if (curLayer != NULL)
			m_LastLayer = curLayer;
	}

}

void Packet::reallocateRawData(size_t newSize)
{
	LOG_DEBUG("Allocating packet to new size: %d", newSize);

	// allocate a new array with size newSize
	m_MaxPacketLen = newSize;
	uint8_t* newData = new uint8_t[m_MaxPacketLen];
	memset(newData, 0, m_MaxPacketLen);

	// set the new array to RawPacket
	m_RawPacket->reallocateData(newData);

	// set all data pointers in layers to the new array address
	const uint8_t* dataPtr = m_RawPacket->getRawData();

	Layer* curLayer = m_FirstLayer;
	while (curLayer != NULL)
	{
		LOG_DEBUG("Setting new data pointer to layer '%s'", typeid(curLayer).name());
		curLayer->m_Data = (uint8_t*)dataPtr;
		dataPtr += curLayer->getHeaderLen();
		curLayer = curLayer->getNextLayer();
	}
}

bool Packet::addLayer(Layer* newLayer)
{
	return insertLayer(m_LastLayer, newLayer);
}

bool Packet::insertLayer(Layer* prevLayer, Layer* newLayer)
{
	if (newLayer->m_DataAllocatedToPacket)
	{
		LOG_ERROR("Layer is already allocated to another packet. Cannot use layer in more than one packet");
		return false;
	}

	if (m_RawPacket->getRawDataLen() + newLayer->getHeaderLen() > m_MaxPacketLen)
	{
		// reallocate to maximum value of: twice the max size of the packet or max size + new required length
		if (m_RawPacket->getRawDataLen() + newLayer->getHeaderLen() > m_MaxPacketLen*2)
			reallocateRawData(m_RawPacket->getRawDataLen() + newLayer->getHeaderLen() + m_MaxPacketLen);
		else
			reallocateRawData(m_MaxPacketLen*2);
	}

	size_t appendDataLen = newLayer->getHeaderLen();

	// insert layer data to raw packet
	int indexToInsertData = 0;
	if (prevLayer != NULL)
		indexToInsertData = prevLayer->m_Data+prevLayer->getHeaderLen() - m_RawPacket->getRawData();
	m_RawPacket->insertData(indexToInsertData, newLayer->m_Data, appendDataLen);

	//delete previous layer data
	delete[] newLayer->m_Data;

	// add layer to layers linked list
	if (prevLayer != NULL)
	{
		newLayer->setNextLayer(prevLayer->getNextLayer());
		newLayer->setPrevLayer(prevLayer);
		prevLayer->setNextLayer(newLayer);
	}
	else //prevLayer == NULL
	{
		newLayer->setNextLayer(m_FirstLayer);
		if (m_FirstLayer != NULL)
			m_FirstLayer->setPrevLayer(newLayer);
		m_FirstLayer = newLayer;
	}

	if (newLayer->getNextLayer() == NULL)
		m_LastLayer = newLayer;

	// assign layer with this packet only
	newLayer->m_DataAllocatedToPacket = true;

	// re-calculate all layers data ptr and data length
	const uint8_t* dataPtr = m_RawPacket->getRawData();
	int dataLen = m_RawPacket->getRawDataLen();

	Layer* curLayer = m_FirstLayer;
	while (curLayer != NULL)
	{
		curLayer->m_Data = (uint8_t*)dataPtr;
		curLayer->m_DataLen = dataLen;
		dataPtr += curLayer->getHeaderLen();
		dataLen -= curLayer->getHeaderLen();
		curLayer = curLayer->getNextLayer();
	}

	// add layer protocol to protocol collection
	m_ProtocolTypes |= newLayer->getProtocol();
	return true;
}

bool Packet::removeLayer(Layer* layer)
{
	if (layer == NULL)
	{
		LOG_ERROR("Layer is NULL");
		return false;
	}

	// verify layer is allocated to a packet
	if (!layer->m_DataAllocatedToPacket)
	{
		LOG_ERROR("Layer isn't allocated to any packet");
		return false;
	}

	// verify layer is allocated to *this* packet
	Layer* curLayer = layer;
	while (curLayer->m_PrevLayer != NULL)
		curLayer = curLayer->m_PrevLayer;
	if (curLayer != m_FirstLayer)
	{
		LOG_ERROR("Layer isn't allocated to this packet");
		return false;
	}

	// remove data from raw packet
	size_t numOfBytesToRemove = layer->getHeaderLen();
	int indexOfDataToRemove = layer->m_Data - m_RawPacket->getRawData();
	if (!m_RawPacket->removeData(indexOfDataToRemove, numOfBytesToRemove))
	{
		LOG_ERROR("Couldn't remove data from packet");
		return false;
	}

	// remove layer from layers linked list
	if (layer->m_PrevLayer != NULL)
		layer->m_PrevLayer->setNextLayer(layer->m_NextLayer);
	if (layer->m_NextLayer != NULL)
		layer->m_NextLayer->setPrevLayer(layer->m_PrevLayer);

	// take care of head and tail ptrs
	if (m_FirstLayer == layer)
		m_FirstLayer = layer->m_NextLayer;
	if (m_LastLayer == layer)
		m_LastLayer = layer->m_PrevLayer;
	layer->setNextLayer(NULL);
	layer->setPrevLayer(NULL);

	// re-calculate all layers data ptr and data length
	const uint8_t* dataPtr = m_RawPacket->getRawData();
	int dataLen = m_RawPacket->getRawDataLen();

	curLayer = m_FirstLayer;
	bool anotherLayerWithSameProtocolExists = false;
	while (curLayer != NULL)
	{
		curLayer->m_Data = (uint8_t*)dataPtr;
		curLayer->m_DataLen = dataLen;
		if (curLayer->getProtocol() == layer->getProtocol())
			anotherLayerWithSameProtocolExists = true;
		dataPtr += curLayer->getHeaderLen();
		dataLen -= curLayer->getHeaderLen();
		curLayer = curLayer->getNextLayer();
	}

	// remove layer protocol from protocol list if necessary
	if (!anotherLayerWithSameProtocolExists)
		m_ProtocolTypes &= ~((uint64_t)layer->getProtocol());

	return true;
}

Layer* Packet::getLayerOfType(ProtocolType type)
{
	if (m_FirstLayer->m_Protocol == type)
		return m_FirstLayer;

	return getNextLayerOfType(m_FirstLayer, type);
}

Layer* Packet::getNextLayerOfType(Layer* after, ProtocolType type)
{
	if (after == NULL)
		return NULL;

	Layer* curLayer = after->getNextLayer();
	while (curLayer != NULL)
	{
		if (curLayer->m_Protocol == type)
			return curLayer;
		else
			curLayer = curLayer->getNextLayer();
	}

	return NULL;
}

void Packet::computeCalculateFields()
{
	Layer* curLayer = m_FirstLayer;
	while (curLayer != NULL)
	{
		curLayer->computeCalculateFields();
		curLayer = curLayer->getNextLayer();
	}
}

Packet::~Packet()
{
	for(std::vector<Layer*>::iterator iter = m_LayersInitialized.begin(); iter != m_LayersInitialized.end(); ++iter)
		delete (*iter);

	if (m_FreeRawPacket)
		delete m_RawPacket;
}

