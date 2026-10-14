#ifndef __K_MACRO_UTILS__
#define __K_MACRO_UTILS__


#define WRAP_METHOD(class_name, method_name, return_type) \
	extern "C" { \
		return_type call_##method_name(class_name* instance) { \
			return instance->method_name(); \
		} \
	}
#define WRAP_METHOD_DEC(class_name, method_name, return_type) \
	extern "C" { \
		return_type call_##method_name(class_name* instance); \
	}


#define C_WRAP_METHOD_DEC(class_name, method_name, return_type, ...) \
	return_type call_##method_name(class_name* instance);





#define GET_C_METHOD(ret, name, ...) \
	extern "C"{ \
		ret name(__VA_ARGS__); \
	}




#define INIT_CLASS(class_name) \
	extern "C" { \
		class_name* create_##class_name() { \
			return new class_name(); \
		} \
		void delete_##class_name(class_name* obj) { \
			delete obj; \
		} \
	}

#define INIT_CLASS_DEC(class_name) \
	extern "C" { \
		class_name* create_##class_name(); \
		void delete_##class_name(class_name* obj); \
	}


	#define C_INIT_CLASS_DEC(class_name) \
	typedef struct class_name class_name; \
	class_name* create_##class_name (); \
	void delete_##class_name (class_name* obj); 



#endif
