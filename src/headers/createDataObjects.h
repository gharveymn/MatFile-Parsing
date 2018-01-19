#ifndef CREATE_DATA_OBJECTS_H
#define CREATE_DATA_OBJECTS_H

#include "getDataObjects.h"

void makeObjectTreeSkeleton(void);
void readTreeNode(Data* object, address_t node_address, address_t heap_address);
void readSnod(Data* object, address_t node_address, address_t heap_address);
Data* connectSubObject(Data* super_object, address_t sub_obj_address, char* sub_obj_name);
uint16_t getNumSymbols(address_t address);

#endif //CREATE_DATA_OBJECTS_H
