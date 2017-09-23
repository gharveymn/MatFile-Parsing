#include "mapping.h"


Superblock getSuperblock(void)
{
	byte* superblock_pointer = findSuperblock();
	Superblock s_block = fillSuperblock(superblock_pointer);
	return s_block;
}


byte* findSuperblock(void)
{
	//Assuming that superblock is in first 8 512 byte chunks
	byte* chunk_start = navigateTo(0, alloc_gran, TREE);
	uint16_t chunk_address = 0;
	
	while(strncmp(FORMAT_SIG, (char*)chunk_start, 8) != 0 && chunk_address < alloc_gran)
	{
		chunk_start += 512;
		chunk_address += 512;
	}
	
	if(chunk_address >= alloc_gran)
	{
		readMXError("getmatvar:superblockNotFoundError", "Couldn't find superblock in first 8 512-byte chunks.\n\n");
	}
	
	return chunk_start;
}


Superblock fillSuperblock(byte* superblock_pointer)
{
	Superblock s_block;
	//get stuff from superblock, for now assume consistent versions of stuff
	s_block.size_of_offsets = (uint8_t)getBytesAsNumber(superblock_pointer + 13, 1, META_DATA_BYTE_ORDER);
	s_block.size_of_lengths = (uint8_t)getBytesAsNumber(superblock_pointer + 14, 1, META_DATA_BYTE_ORDER);
	s_block.leaf_node_k = (uint16_t)getBytesAsNumber(superblock_pointer + 16, 2, META_DATA_BYTE_ORDER);
	s_block.internal_node_k = (uint16_t)getBytesAsNumber(superblock_pointer + 18, 2, META_DATA_BYTE_ORDER);
	s_block.base_address = getBytesAsNumber(superblock_pointer + 24, s_block.size_of_offsets, META_DATA_BYTE_ORDER);
	
	//read scratchpad space
	byte* sps_start = superblock_pointer + 80;
	root_trio.parent_obj_header_address = UNDEF_ADDR;
	root_trio.tree_address = getBytesAsNumber(sps_start, s_block.size_of_offsets, META_DATA_BYTE_ORDER) + s_block.base_address;
	root_trio.heap_address = getBytesAsNumber(sps_start + s_block.size_of_offsets, s_block.size_of_offsets, META_DATA_BYTE_ORDER) + s_block.base_address;
	s_block.root_tree_address = root_trio.tree_address;
	
	return s_block;
}


void freeAllMaps(void)
{
	for(int i = 0; i < NUM_TREE_MAPS; i++)
	{
		if(tree_maps[i].used == TRUE)
		{
			freeMap(tree_maps[i]);
		}
	}
	
	for(int i = 0; i < NUM_HEAP_MAPS; i++)
	{
		if(heap_maps[i].used == TRUE)
		{
			freeMap(heap_maps[i]);
		}
	}
}


void freeMap(MemMap map)
{
	map.used = FALSE;
	if(munmap(map.map_start, map.bytes_mapped) != 0)
	{
		readMXError("getmatvar:badMunmapError", "munmap() unsuccessful in freeMap(). Check errno %s\n\n", strerror(errno));
	}
}


byte* navigateTo(uint64_t address, uint64_t bytes_needed, int map_type)
{
	
	MemMap* these_maps;
	switch(map_type)
	{
		case TREE:
			these_maps = tree_maps;
			break;
		case HEAP:
			these_maps = heap_maps;
			break;
		default:
			these_maps = tree_maps;
	}
	
	for(int i = 0; i < map_nums[map_type]; i++)
	{
		if(address >= these_maps[i].offset && address + bytes_needed <= these_maps[i].offset + these_maps[i].bytes_mapped && these_maps[i].used == TRUE)
		{
			return these_maps[i].map_start + address - these_maps[i].offset;
		}
	}
	
	//only remap if we really need to (this removes the need for checks/headaches inside main functions)
	int map_index = map_queue_fronts[map_type];
	
	//unmap current page
	if(these_maps[map_index].used == TRUE)
	{
		freeMap(these_maps[map_index]);
	}
	
	//map new page at needed location
	//TODO check if we even need to do this... I don't think so
	size_t offset_denom = alloc_gran < file_size? alloc_gran : file_size;
	these_maps[map_index].offset = (OffsetType)((address/offset_denom)*offset_denom);
	these_maps[map_index].bytes_mapped = address - these_maps[map_index].offset + bytes_needed;
	these_maps[map_index].bytes_mapped = these_maps[map_index].bytes_mapped < file_size - these_maps[map_index].offset? these_maps[map_index].bytes_mapped : file_size - these_maps[map_index].offset;
	these_maps[map_index].map_start = mmap(NULL, these_maps[map_index].bytes_mapped, PROT_READ, MAP_PRIVATE, fd, these_maps[map_index].offset);
	
	these_maps[map_index].used = TRUE;
	if(these_maps[map_index].map_start == NULL || these_maps[map_index].map_start == MAP_FAILED)
	{
		these_maps[map_index].used = FALSE;
		readMXError("getmatvar:mmapUnsuccessfulError", "mmap() unsuccessful in navigateTo(). Check errno %s\n\n", strerror(errno));
	}
	
	map_queue_fronts[map_type] = (map_queue_fronts[map_type] + 1)%map_nums[map_type];
	
	return these_maps[map_index].map_start + address - these_maps[map_index].offset;
}


byte* navigatePolitely(uint64_t address, uint64_t bytes_needed)
{
	
	size_t start_page = address/alloc_gran;
	size_t end_page = (address + bytes_needed)/alloc_gran; //INCLUSIVE
	
	
	/*-----------------------------------------WINDOWS-----------------------------------------*/
#if !((defined(_WIN32) || defined(WIN32) || defined(_WIN64)) && !defined __CYGWIN__)
	
	//in Windows the .is_mapped becomes a flag for if the mapping originally came from this object
	//if there is a map available the map_start and map_end addresses indicate where the start and end are
	//if the object is not associated to a map at all the map_start and map_end addresses will be UNDEF_ADDR
	
	while(TRUE)
	{
		
		//this covers cases:
		//				already mapped, use this map
		//				already mapped, in use, wait for lock and threads using to finish to remap
		//				already mapped, in use, remapped while waiting, use that map
		//				not mapped, acquire lock to map
		//				not mapped, mapped while waiting for map lock, use that map
		//				not mapped
		
		
		//check if we have continuous mapping available (if yes then return pointer)
		if(page_objects[start_page].map_base <= address && address + bytes_needed <= page_objects[start_page].map_end)
		{

#ifdef DO_MEMDUMP
			memdump("R");
#endif
			
			pthread_mutex_lock(&page_objects[start_page].lock);
			page_objects[start_page].num_using++;
			pthread_mutex_unlock(&page_objects[start_page].lock);
			
			return page_objects[start_page].pg_start_p + (address - page_objects[start_page].pg_start_a);
			
		}
		else
		{
			
			//acquire lock if we need to remap
			//lock the if so there isn't deadlock
			pthread_spin_lock(&if_lock);
			if(page_objects[start_page].num_using == 0)
			{
				pthread_mutex_lock(&page_objects[start_page].lock);
				
				//the state may have changed while acquiring the lock, so check again
				if(page_objects[start_page].num_using == 0)
				{
					page_objects[start_page].num_using++;
					pthread_spin_unlock(&if_lock);
					break;
				}
				else
				{
					pthread_mutex_unlock(&page_objects[start_page].lock);
				}
			}
			pthread_spin_unlock(&if_lock);
			
		}
		
	}
	
	
	
	//if this page has already been mapped unmap since we can't fit
	if(page_objects[start_page].is_mapped == TRUE)
	{
		if(munmap(page_objects[start_page].pg_start_p, 0) != 0)
		{
			readMXError("getmatvar:badMunmapError", "munmap() unsuccessful in navigatePolitely(). Check errno %d\n\n", errno);
		}
		
		page_objects[start_page].is_mapped = FALSE;
		page_objects[start_page].pg_start_p = NULL;
		page_objects[start_page].map_base = UNDEF_ADDR;
		page_objects[start_page].map_end = UNDEF_ADDR;

#ifdef DO_MEMDUMP
		memdump("U");
#endif
	
	}
	
	page_objects[start_page].pg_start_p = mmap(NULL, page_objects[end_page].pg_end_a - page_objects[start_page].pg_start_a, PROT_READ, MAP_PRIVATE, fd, page_objects[start_page].pg_start_a);
	
	if(page_objects[start_page].pg_start_p == NULL || page_objects[start_page].pg_start_p == MAP_FAILED)
	{
		readMXError("getmatvar:mmapUnsuccessfulError", "mmap() unsuccessful in navigatePolitely(). Check errno %d\n\n", errno);
	}
	
	page_objects[start_page].is_mapped = TRUE;
	page_objects[start_page].map_base = page_objects[start_page].pg_start_a;
	page_objects[start_page].map_end = page_objects[end_page].pg_end_a;

#ifdef DO_MEMDUMP
	memdump("M");
#endif
	
	pthread_mutex_unlock(&page_objects[start_page].lock);
	
	return page_objects[start_page].pg_start_p + (address - page_objects[start_page].pg_start_a);

#else /*-----------------------------------------UNIX-----------------------------------------*/
	
	//TODO this doesn't work right now. Try Windows!
	
//	while(TRUE)
//	{
		
		//this can change while waiting
		if(page_objects[start_page].map_base <= address && address + bytes_needed <= page_objects[start_page].map_end)
		{
			
			//If this is true I'm going to try to acquire all of the locks for the pages I need
			//and signal my intent to use them. If I can't get one of the locks then
			//I'll signal that I'm done and release all of them
			
			bool_t is_continuous = TRUE;
			for(size_t i = start_page; i <= end_page; i++)
			{
				
				//passive lock mechanism
				if(pthread_mutex_trylock(&page_objects[i].lock) == EBUSY)
				{
					for(size_t j = i - 1; j >= start_page; j--)
					{
						pthread_mutex_unlock(&page_objects[j].lock);
					}
					is_continuous = FALSE;
					break;
				}
				
				
				if(page_objects[i].map_base != page_objects[start_page].map_base || page_objects[i].is_mapped == FALSE)
				{
					//we acquired this lock as well, so make sure j = i is the start
					for(size_t j = i; j >= start_page; j--)
					{
						pthread_mutex_unlock(&page_objects[j].lock);
					}
					is_continuous = FALSE;
					break;
				}
			}
			
			//if is_continuous true I should still have all the locks in this address space
			
			if(is_continuous == TRUE)
			{

#ifdef DO_MEMDUMP
				memdump("R");
#endif
				
				//I confirmed that I added my intent to each of the pages, now I can release
				//all of my locks knowing that the pages cant be remapped while im using them
				
				for(size_t i = start_page; i <= end_page; i++)
				{
					page_objects[i].num_using++;
					pthread_mutex_unlock(&page_objects[i].lock);
				}
				
				return page_objects[start_page].pg_start_p + (address - page_objects[start_page].pg_start_a);
			}
		}
		else
		{
			
			//same method, try to acquire all of the locks, but give priority to the reusers, so don't lock on check
			pthread_spin_lock(&if_lock);
			
			//this section is locked, so safe to check all nums
			
			//quick entry check
//			if(page_objects[start_page].num_using == 0)
//			{
//				bool_t is_clear = TRUE;
//				for(size_t i = start_page; i <= end_page; i++)
//				{
//					if(page_objects[i].num_using != 0)
//					{
//						is_clear = FALSE;
//						break;
//					}
//				}
//
//				if(is_clear == TRUE)
//				{
//
//					//ensure locking of the rest of the pages
//
//					for(size_t i = start_page; i <= end_page; i++)
//					{
//						pthread_mutex_lock(&page_objects[i].lock);
//					}
//
//					//recheck because some object may have started using it while waiting for locks
//					while(TRUE)
//					{
//						bool_t recheck_is_clear = TRUE;
//						for(size_t i = start_page; i <= end_page; i++)
//						{
//							if(page_objects[i].num_using != 0)
//							{
//								//these pages can't get remapped by another thread because of if_lock, so unlock them so they can
//								//signal they are done being used
//								recheck_is_clear = FALSE;
//								page_objects[i].needs_relock = TRUE;
//								pthread_mutex_unlock(&page_objects[i].lock);
//							}
//							else
//							{
//								page_objects[i].num_using++;
//							}
//						}
//
//						if(recheck_is_clear == TRUE)
//						{
//							break;
//						}
//
//
//						//after unlocking for processes to finish relock those pages
//						for(size_t i = start_page; i <= end_page; i++)
//						{
//							if(page_objects[i].needs_relock == TRUE)
//							{
//								while(TRUE)
//								{
//									//loop until we absolutely have a clean lock
//									pthread_mutex_lock(&page_objects[i].lock);
//									if(page_objects[i].num_using != 0)
//									{
//										break;
//									}
//									else
//									{
//										pthread_mutex_unlock(&page_objects[i].lock);
//									}
//
//								}
//							}
//							page_objects[i].needs_relock = FALSE;
//
//						}
//
//					}
//
//					pthread_spin_unlock(&if_lock);
//					break;
//
//
//				}
//
//			}

			for(size_t i = start_page; i <= end_page; i++)
			{
				//note that no other threads can remap while doing this
				while(TRUE)
				{
					pthread_mutex_lock(&page_objects[i].lock);
					if(page_objects[i].num_using != 0)
					{
						pthread_mutex_unlock(&page_objects[i].lock);
					}
					else
					{
						page_objects[i].num_using++;
						break;
					}
				}
			}
			
			pthread_spin_unlock(&if_lock);
		
		}
		
	//}
	
	//implicit else
	
	for(size_t i = start_page; i <= end_page; i++)
	{
		if(page_objects[i].is_mapped == TRUE && page_objects[i].pg_start_p != NULL)
		{
			if(munmap(page_objects[i].pg_start_p, page_objects[i].pg_end_a - page_objects[i].pg_start_a) != 0)
			{
				readMXError("getmatvar:badMunmapError", "munmap() unsuccessful in navigatePolitely(). Check errno %d\n\n", errno);
			}
			page_objects[i].is_mapped = FALSE;
			page_objects[i].pg_start_p = NULL;
			page_objects[i].map_base = UNDEF_ADDR;
			page_objects[i].map_end = UNDEF_ADDR;
		}
	}
	
#ifdef DO_MEMDUMP
		memdump("U");
#endif
	
	page_objects[start_page].pg_start_p = mmap(NULL, page_objects[end_page].pg_end_a - page_objects[start_page].pg_start_a, PROT_READ, MAP_PRIVATE, fd, page_objects[start_page].pg_start_a);
	
	if(page_objects[start_page].pg_start_p == NULL || page_objects[start_page].pg_start_p == MAP_FAILED)
	{
		readMXError("getmatvar:mmapUnsuccessfulError", "mmap() unsuccessful in navigatePolitely(). Check errno %d\n\n", errno);
	}
	
	page_objects[start_page].is_mapped = TRUE;
	page_objects[start_page].map_base = page_objects[start_page].pg_start_a;
	page_objects[start_page].map_end = page_objects[end_page].pg_end_a;
	
	for(size_t i = start_page + 1; i <= end_page; i++)
	{
		page_objects[i].is_mapped = TRUE;
		page_objects[i].pg_start_p = page_objects[i - 1].pg_start_p + alloc_gran;
		page_objects[i].map_base = page_objects[start_page].pg_start_a;
		page_objects[i].map_end = page_objects[end_page].pg_end_a;
    }
	
#ifdef DO_MEMDUMP
		memdump("M");
#endif
	
	pthread_mutex_unlock(&page_objects[start_page].lock);
	
	return page_objects[start_page].pg_start_p + (address - page_objects[start_page].pg_start_a);

#endif

}


void releasePages(uint64_t address, uint64_t bytes_needed)
{
	
	//call this after done with using the pointer
	size_t start_page = address/alloc_gran;
	size_t end_page = (address + bytes_needed)/alloc_gran; //INCLUSIVE
	
	/*-----------------------------------------WINDOWS-----------------------------------------*/
#if (defined(_WIN32) || defined(WIN32) || defined(_WIN64)) && !defined __CYGWIN__
	
	pthread_mutex_lock(&page_objects[start_page].lock);
	page_objects[start_page].num_using--;
	pthread_mutex_unlock(&page_objects[start_page].lock);

#else /*-----------------------------------------UNIX-----------------------------------------*/
	
	//release locks
	for(size_t i = start_page; i <= end_page; i++)
	{
		pthread_mutex_lock(&page_objects[i].lock);
		page_objects[i].num_using--;
		pthread_mutex_unlock(&page_objects[i].lock);
	}

#endif

}


//OBSOLETE (unused)
byte* navigateWithMapIndex(uint64_t address, uint64_t bytes_needed, int map_type, int map_index)
{
	
	MemMap* these_maps;
	switch(map_type)
	{
		case TREE:
			these_maps = tree_maps;
			break;
		case HEAP:
			these_maps = heap_maps;
			break;
		default:
			these_maps = tree_maps;
	}
	
	if(!(address >= these_maps[map_index].offset && address + bytes_needed <= these_maps[map_index].offset + these_maps[map_index].bytes_mapped) || these_maps[map_index].used == FALSE)
	{
		
		//unmap current page
		if(these_maps[map_index].used == TRUE)
		{
			freeMap(these_maps[map_index]);
		}
		
		//map new page at needed location
		size_t offset_denom = alloc_gran < file_size? alloc_gran : file_size;
		these_maps[map_index].offset = (OffsetType)((address/offset_denom)*offset_denom);
		these_maps[map_index].bytes_mapped = address - these_maps[map_index].offset + bytes_needed;
		these_maps[map_index].bytes_mapped =
				these_maps[map_index].bytes_mapped < file_size - these_maps[map_index].offset? these_maps[map_index].bytes_mapped : file_size - these_maps[map_index].offset;
		these_maps[map_index].map_start = mmap(NULL, these_maps[map_index].bytes_mapped, PROT_READ, MAP_PRIVATE, fd, these_maps[map_index].offset);
		these_maps[map_index].used = TRUE;
		if(these_maps[map_index].map_start == NULL || these_maps[map_index].map_start == MAP_FAILED)
		{
			these_maps[map_index].used = FALSE;
			readMXError("getmatvar:mmapUnsuccessfulError", "mmap() unsuccessful in navigateWithMapIndex(). Check errno %s\n\n", strerror(errno));
		}
		
	}
	
	return these_maps[map_index].map_start + address - these_maps[map_index].offset;
	
}


void readTreeNode(byte* tree_pointer, AddrTrio* this_trio)
{
	AddrTrio* trio;
	uint16_t entries_used = 0;
	uint64_t left_sibling_address, right_sibling_address;
	
	entries_used = (uint16_t)getBytesAsNumber(tree_pointer + 6, 2, META_DATA_BYTE_ORDER);
	
	//assuming keys do not contain pertinent information
	left_sibling_address = getBytesAsNumber(tree_pointer + 8, s_block.size_of_offsets, META_DATA_BYTE_ORDER);
	if(left_sibling_address != UNDEF_ADDR)
	{
		trio = malloc(sizeof(AddrTrio));
		trio->parent_obj_header_address = this_trio->parent_obj_header_address;
		trio->tree_address = left_sibling_address + s_block.base_address;
		trio->heap_address = this_trio->heap_address;
		enqueue(addr_queue, trio);
	}
	
	right_sibling_address = getBytesAsNumber(tree_pointer + 8 + s_block.size_of_offsets, s_block.size_of_offsets, META_DATA_BYTE_ORDER);
	if(right_sibling_address != UNDEF_ADDR)
	{
		trio = malloc(sizeof(AddrTrio));
		trio->parent_obj_header_address = this_trio->parent_obj_header_address;;
		trio->tree_address = right_sibling_address + s_block.base_address;
		trio->heap_address = this_trio->heap_address;
		enqueue(addr_queue, trio);
	}
	
	//group node B-Tree traversal (version 0)
	int key_size = s_block.size_of_lengths;
	byte* key_pointer = tree_pointer + 8 + 2*s_block.size_of_offsets;
	for(int i = 0; i < entries_used; i++)
	{
		trio = malloc(sizeof(AddrTrio));
		trio->parent_obj_header_address = this_trio->parent_obj_header_address;
		trio->tree_address = getBytesAsNumber(key_pointer + key_size, s_block.size_of_offsets, META_DATA_BYTE_ORDER) + s_block.base_address;
		trio->heap_address = this_trio->heap_address;
		
		//in the case where we encounter a struct but have more items ahead
		priorityEnqueue(addr_queue, trio);
		
		key_pointer += key_size + s_block.size_of_offsets;
		
	}
	
}


void readSnod(byte* snod_pointer, byte* heap_pointer, AddrTrio* parent_trio, AddrTrio* this_trio, bool_t get_top_level)
{
	uint16_t num_symbols = (uint16_t)getBytesAsNumber(snod_pointer + 6, 2, META_DATA_BYTE_ORDER);
	uint32_t cache_type;
	AddrTrio* trio;
	char* var_name = peekQueue(varname_queue, QUEUE_FRONT);
	
	uint64_t heap_data_segment_size = getBytesAsNumber(heap_pointer + 8, s_block.size_of_lengths, META_DATA_BYTE_ORDER);
	uint64_t heap_data_segment_address = getBytesAsNumber(heap_pointer + 8 + 2*s_block.size_of_lengths, s_block.size_of_offsets, META_DATA_BYTE_ORDER) + s_block.base_address;
	byte* heap_data_segment_pointer = navigateTo(heap_data_segment_address, heap_data_segment_size, HEAP);
	
	//get to entries
	int sym_table_entry_size = 2*s_block.size_of_offsets + 4 + 4 + 16;
	
	for(int i = 0, is_done = FALSE; i < num_symbols && is_done != TRUE; i++)
	{
		SNODEntry* snod_entry = malloc(sizeof(SNODEntry));
		snod_entry->name_offset = getBytesAsNumber(snod_pointer + 8 + i*sym_table_entry_size, s_block.size_of_offsets, META_DATA_BYTE_ORDER);
		snod_entry->parent_obj_header_address = this_trio->parent_obj_header_address;
		snod_entry->this_obj_header_address =
				getBytesAsNumber(snod_pointer + 8 + i*sym_table_entry_size + s_block.size_of_offsets, s_block.size_of_offsets, META_DATA_BYTE_ORDER) + s_block.base_address;
		strcpy(snod_entry->name, (char*)(heap_data_segment_pointer + snod_entry->name_offset));
		cache_type = (uint32_t)getBytesAsNumber(snod_pointer + 8 + 2*s_block.size_of_offsets + sym_table_entry_size*i, 4, META_DATA_BYTE_ORDER);
		snod_entry->parent_tree_address = parent_trio->tree_address;
		snod_entry->this_tree_address = this_trio->tree_address;
		snod_entry->sub_tree_address = UNDEF_ADDR;
		
		//check if we have found the object we're looking for or if we are just adding subobjects
		if((variable_found == TRUE || (var_name != NULL && strcmp(var_name, snod_entry->name) == 0)) && strncmp(snod_entry->name, "#", 1) != 0)
		{
			if(variable_found == FALSE)
			{
				
				resetQueue(header_queue);
				resetQueue(addr_queue);
				dequeue(varname_queue);
				
				//means this was the last token, so we've found the variable we want
				if(varname_queue->length == 0)
				{
					variable_found = TRUE;
					is_done = TRUE;
				}
				
			}
			
			
			enqueue(header_queue, snod_entry);
			
			//if the variable has been found we should keep going down the tree for that variable
			//all items in the queue should only be subobjects so this is safe
			if(cache_type == 1 && get_top_level == FALSE)
			{
				
				//if another tree exists for this object, put it on the queue
				trio = malloc(sizeof(AddrTrio));
				trio->parent_obj_header_address = snod_entry->this_obj_header_address;
				trio->tree_address =
						getBytesAsNumber(snod_pointer + 8 + 2*s_block.size_of_offsets + 8 + sym_table_entry_size*i, s_block.size_of_offsets, META_DATA_BYTE_ORDER) + s_block.base_address;
				trio->heap_address =
						getBytesAsNumber(snod_pointer + 8 + 3*s_block.size_of_offsets + 8 + sym_table_entry_size*i, s_block.size_of_offsets, META_DATA_BYTE_ORDER) + s_block.base_address;
				snod_entry->sub_tree_address = trio->tree_address;
				priorityEnqueue(addr_queue, trio);
				parseHeaderTree(FALSE);
				snod_pointer = navigateTo(this_trio->tree_address, default_bytes, TREE);
				heap_pointer = navigateTo(this_trio->heap_address, default_bytes, HEAP);
				heap_data_segment_pointer = navigateTo(heap_data_segment_address, heap_data_segment_size, HEAP);
				
			}
			else if(cache_type == 2  && get_top_level == FALSE)
			{
				//this object is a symbolic link, the name is stored in the heap at the address indicated in the scratch pad
				snod_entry->name_offset = getBytesAsNumber(snod_pointer + 8 + 2*s_block.size_of_offsets + 8 + sym_table_entry_size*i, 4, META_DATA_BYTE_ORDER);
				strcpy(snod_entry->name, (char*)(heap_pointer + 8 + 2*s_block.size_of_lengths + s_block.size_of_offsets + snod_entry->name_offset));
			}

		}
		else
		{
			free(snod_entry);
		}

	}
	
	
}


void freeDataObject(void* object)
{
	Data* data_object = (Data*)object;
	
	if(data_object->data_arrays.is_mx_used != TRUE)
	{
		if(data_object->data_arrays.ui8_data != NULL)
		{
			mxFree(data_object->data_arrays.ui8_data);
			data_object->data_arrays.ui8_data = NULL;
		}
		
		if(data_object->data_arrays.i8_data != NULL)
		{
			mxFree(data_object->data_arrays.i8_data);
			data_object->data_arrays.i8_data = NULL;
		}
		
		if(data_object->data_arrays.ui16_data != NULL)
		{
			mxFree(data_object->data_arrays.ui16_data);
			data_object->data_arrays.ui16_data = NULL;
		}
		
		if(data_object->data_arrays.i16_data != NULL)
		{
			mxFree(data_object->data_arrays.i16_data);
			data_object->data_arrays.i16_data = NULL;
		}
		
		if(data_object->data_arrays.ui32_data != NULL)
		{
			mxFree(data_object->data_arrays.ui32_data);
			data_object->data_arrays.ui32_data = NULL;
		}
		
		if(data_object->data_arrays.i32_data != NULL)
		{
			mxFree(data_object->data_arrays.i32_data);
			data_object->data_arrays.i32_data = NULL;
		}
		
		if(data_object->data_arrays.ui64_data != NULL)
		{
			mxFree(data_object->data_arrays.ui64_data);
			data_object->data_arrays.ui64_data = NULL;
		}
		
		if(data_object->data_arrays.i64_data != NULL)
		{
			mxFree(data_object->data_arrays.i64_data);
			data_object->data_arrays.i64_data = NULL;
		}
		
		if(data_object->data_arrays.single_data != NULL)
		{
			mxFree(data_object->data_arrays.single_data);
			data_object->data_arrays.single_data = NULL;
		}
		
		if(data_object->data_arrays.double_data != NULL)
		{
			mxFree(data_object->data_arrays.double_data);
			data_object->data_arrays.double_data = NULL;
		}
	}
	
	if(data_object->data_arrays.sub_object_header_offsets != NULL)
	{
		free(data_object->data_arrays.sub_object_header_offsets);
		data_object->data_arrays.sub_object_header_offsets = NULL;
	}
	
	for(int j = 0; j < data_object->chunked_info.num_filters; j++)
	{
		free(data_object->chunked_info.filters[j].client_data);
		data_object->chunked_info.filters[j].client_data = NULL;
	}
	
	if(data_object->sub_objects != NULL)
	{
		free(data_object->sub_objects);
		data_object->sub_objects = NULL;
	}
	
	
	free(data_object);
	
}


void freeDataObjectTree(Data* super_object)
{
	
	if(super_object->data_arrays.is_mx_used != TRUE && super_object->data_arrays.ui8_data != NULL)
	{
		mxFree(super_object->data_arrays.ui8_data);
		super_object->data_arrays.ui8_data = NULL;
	}
	
	if(super_object->data_arrays.is_mx_used != TRUE && super_object->data_arrays.i8_data != NULL)
	{
		mxFree(super_object->data_arrays.i8_data);
		super_object->data_arrays.i8_data = NULL;
	}
	
	if(super_object->data_arrays.is_mx_used != TRUE && super_object->data_arrays.ui16_data != NULL)
	{
		mxFree(super_object->data_arrays.ui16_data);
		super_object->data_arrays.ui16_data = NULL;
	}
	
	if(super_object->data_arrays.is_mx_used != TRUE && super_object->data_arrays.i16_data != NULL)
	{
		mxFree(super_object->data_arrays.i16_data);
		super_object->data_arrays.i16_data = NULL;
	}
	
	if(super_object->data_arrays.is_mx_used != TRUE && super_object->data_arrays.ui32_data != NULL)
	{
		mxFree(super_object->data_arrays.ui32_data);
		super_object->data_arrays.ui32_data = NULL;
	}
	
	if(super_object->data_arrays.is_mx_used != TRUE && super_object->data_arrays.i32_data != NULL)
	{
		mxFree(super_object->data_arrays.i32_data);
		super_object->data_arrays.i32_data = NULL;
	}
	
	if(super_object->data_arrays.is_mx_used != TRUE && super_object->data_arrays.ui64_data != NULL)
	{
		mxFree(super_object->data_arrays.ui64_data);
		super_object->data_arrays.ui64_data = NULL;
	}
	
	if(super_object->data_arrays.is_mx_used != TRUE && super_object->data_arrays.i64_data != NULL)
	{
		mxFree(super_object->data_arrays.i64_data);
		super_object->data_arrays.i64_data = NULL;
	}
	
	if(super_object->data_arrays.is_mx_used != TRUE && super_object->data_arrays.single_data != NULL)
	{
		mxFree(super_object->data_arrays.single_data);
		super_object->data_arrays.single_data = NULL;
	}
	
	if(super_object->data_arrays.is_mx_used != TRUE && super_object->data_arrays.double_data != NULL)
	{
		mxFree(super_object->data_arrays.double_data);
		super_object->data_arrays.double_data = NULL;
	}
	
	if(super_object->data_arrays.sub_object_header_offsets != NULL)
	{
		free(super_object->data_arrays.sub_object_header_offsets);
		super_object->data_arrays.sub_object_header_offsets = NULL;
	}
	
	for(int j = 0; j < super_object->chunked_info.num_filters; j++)
	{
		free(super_object->chunked_info.filters[j].client_data);
		super_object->chunked_info.filters[j].client_data = NULL;
	}
	
	if(super_object->sub_objects != NULL)
	{
		for(int j = 0; j < super_object->num_sub_objs; j++)
		{
			freeDataObjectTree(super_object->sub_objects[j]);
		}
		free(super_object->sub_objects);
		super_object->sub_objects = NULL;
	}
	
	
	free(super_object);
	super_object = NULL;
	
}


void endHooks(void)
{
	freeAllMaps();
	
	freeQueue(addr_queue);
	freeQueue(varname_queue);
	freeQueue(header_queue);
	
	if(fd >= 0)
	{
		close(fd);
	}
	
}