struct event;

typedef enum {
	EVENT_READ,
	EVENT_WRITE,
	EVENT_RDWR,
} event_t;

typedef	void	event_process_t(struct event *);

struct event {
	int		 fd;
	int		 index;
	event_t		 rdwr;
	event_process_t	*process;
	void		*data;
};

typedef	int	event_module_add_t(struct event *);
typedef int	event_module_init_t(void);
typedef void	event_module_fini_t(void);
typedef int	event_module_process_t(u_long);
struct event_module {
	event_module_add_t	*add;
	event_module_add_t	*del;
	event_module_add_t	*enable;
	event_module_add_t	*disable;
	event_module_process_t	*process;
	event_module_init_t	*init;
	event_module_fini_t	*fini;
};

extern struct event_module event_module;
