#include "headers/cleanup.h"


void freeVarname(void* vn)
{
	char* varname = (char*)vn;
	if(varname != NULL && strcmp(varname, "\0") != 0)
	{
		free(varname);
	}
}


void freeDataObject(void* object)
{
	Data* data_object = (Data*)object;
	
	if(data_object->data_arrays.is_mx_used != TRUE)
	{
		if(data_object->data_arrays.data != NULL)
		{
			mxFree(data_object->data_arrays.data);
			data_object->data_arrays.data = NULL;
		}
	}
	
	if(data_object->names.short_name != NULL)
	{
		free(data_object->names.short_name);
		data_object->names.short_name = NULL;
	}
	
	if(data_object->names.long_name != NULL)
	{
		free(data_object->names.long_name);
		data_object->names.long_name = NULL;
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
		freeQueue(data_object->sub_objects);
		data_object->sub_objects = NULL;
	}
	
	free(data_object);
	
}


void freeDataObjectTree(Data* data_object)
{
	
	if(data_object->data_arrays.is_mx_used != TRUE)
	{
		if(data_object->data_arrays.data != NULL)
		{
			free(data_object->data_arrays.data);
			data_object->data_arrays.data = NULL;
		}
	}
	
	if(data_object->names.short_name != NULL)
	{
		free(data_object->names.short_name);
		data_object->names.short_name = NULL;
	}
	
	if(data_object->names.long_name != NULL)
	{
		free(data_object->names.long_name);
		data_object->names.long_name = NULL;
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
		for(int j = 0; j < data_object->num_sub_objs; j++)
		{
			Data* obj = dequeue(data_object->sub_objects);
			freeDataObjectTree(obj);
		}
		freeQueue(data_object->sub_objects);
		data_object->sub_objects = NULL;
	}
	
	
	free(data_object);
	data_object = NULL;
	
}

void destroyPageObjects(void)
{
	if(page_objects != NULL)
	{
		for(int i = 0; i < num_pages; ++i)
		{
			if(page_objects[i].is_mapped == TRUE)
			{
				
				if(munmap(page_objects[i].pg_start_p, page_objects[i].map_end - page_objects[i].map_base) != 0)
				{
					readMXError("getmatvar:badMunmapError", "munmap() unsuccessful in freeMap(). Check errno %s\n\n", strerror(errno));
				}
				
				page_objects[i].is_mapped = FALSE;
				page_objects[i].pg_start_p = NULL;
				page_objects[i].map_base = UNDEF_ADDR;
				page_objects[i].map_end = UNDEF_ADDR;
				
			}
			
			pthread_mutex_destroy(&page_objects[i].lock);
			
		}
		
		pthread_spin_destroy(&if_lock);
		free(page_objects);
		page_objects = NULL;
		
	}
	
}

void freePageObject(size_t page_index)
{
	
	if(page_objects[page_index].is_mapped == TRUE)
	{
		//second parameter doesnt do anything on windows
		if(munmap(page_objects[page_index].pg_start_p, page_objects[page_index].map_end - page_objects[page_index].map_base) != 0)
		{
			readMXError("getmatvar:badMunmapError", "munmap() unsuccessful in mt_navigateTo(). Check errno %d\n\n", errno);
		}
		
		page_objects[page_index].is_mapped = FALSE;
		page_objects[page_index].pg_start_p = NULL;
		page_objects[page_index].map_base = UNDEF_ADDR;
		page_objects[page_index].map_end = UNDEF_ADDR;

#ifdef DO_MEMDUMP
		memdump("U");
#endif
	
	}
}

void endHooks(void)
{
	
	freeQueue(varname_queue);
	freeQueue(object_queue);
	freeQueue(top_level_objects);
	
	if(parameters.full_variable_names != NULL)
	{
		for(int i = 0; i < parameters.num_vars; i++)
		{
			free(parameters.full_variable_names[i]);
		}
		free(parameters.full_variable_names);
	}
	
	destroyPageObjects();
	
	if(fd >= 0)
	{
		close(fd);
	}
	
}