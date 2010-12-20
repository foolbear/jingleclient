#ifndef __DEFINE_HXX__
#define __DEFINE_HXX__

class Group;
class Buddy;

typedef enum { GENDER_MALE=1, GENDER_FEMALE=0, GENDER_UNKNOWN=10 } Gender;

typedef std::map<buzz::Jid, Buddy *> BuddiesByJid;//Roster or Buddies in Group
typedef std::map<buzz::Jid, Buddy *>::iterator It4BuddiesByJid;

typedef std::map<void *, Buddy*> BuddiesByUI;//All visible Roster
typedef std::map<void *, Buddy*>::iterator It4BuddiesByUI;

typedef std::map<std::string name, Group *> GroupsByName;//All Groups
typedef std::map<std::string name, Group *>::iterator It4GroupsByName;

typedef std::map<void *, Group*> GroupsByUI;//All visible Groups
typedef std::map<void *, Group*>::iterator It4GroupsByUI;

typedef std::vector<buzz::Jid> Resources;//All Resources for the Jid
typedef std::vector<buzz::Jid>::iterator It4Resources;

class Buddy
{
public:
	Buddy() :
		nick_(""),
		nick_by_me_(""),
		gender_(GENDER_UNKNOWN),
		show_(buzz::Status::SHOW_NONE),
		avatar_hash_(""),
		group_(NULL),
		ui_item_(NULL)
	{}
/*	Buddy(const Buddy & r) 
	{
		this->operator=(r);
	}
	Buddy & operator=(const Buddy & r)
	{
		jid_ = r.jid_;
		nick_ = r.nick_;
		nick_by_me_ = r.nick_by_me_;
		gender_ = r.gender_;
		show_ = r.show_;
		status_ = r.status_;
		avatar_hash_ = r.avatar_hash_;
		group_ = r.group_;
		ui_item_ = r.ui_item_;
		return *this;
	}*/
	~Buddy() {}
public:
	const Jid & bare_jid() const { return bare_jid_; }
	const Resources & resources() { return resources_; };
	const std::string & nick() const { return nick_; }
	const std::string & nick_by_me() const { return nick_by_me_; }
	const std::string friendly_name() const 
	{
		if (!nick_by_me_.empty()) return nick_by_me_;
		if (!nick_.empty()) return nick_;
		return bare_jid_.node();
	}
	const Gender gender() const { return gender_; }
	const buzz::Status::Show show() const { return show_; }
	const std::string & status() const { return status_; }
	const std::string & avatar_hash() const { return avatar_hash_; }
	const Group * group() const { return group_; }
	const void * ui_item() const { return ui_item_; }
public:
	void set_bare_id(const Jid & jid) { bare_jid_ = jid.BareJid(); }
	void clear_resources() 
	{ 
		if (!resources_.empty())
			resources_.clear(); 
	}
	void add_resource(const buzz::Jid & resource) 
	{
		resources_.insert(resource);
	}
	void remove_resource(const buzz::Jid & resource) 
	{
		It4Resources it = std::find(resources_.begin(), resources_.end(), resource);
		if (it != resources_.end())
			resources_.erase(it);
	}
	void set_nick(const std::string & nick) { nick_ = nick; }
	void set_nick_by_me(const std::string & nick) { nick_by_me_ = nick; }
	void set_gender(const Gender gender) { gender_ = gender; }
	void set_show(const buzz::Status::Show show) { show_ = show; }
	void set_status(const std::string & status) { status_ = status; }
	void set_group(Group * group) { group_ = group; }
	void set_ui_item(void * ui_item) { ui_item_ = ui_item; }
private:
	buzz::Jid bare_jid_;
	Resources resources_;
	std::string nick_;
	std::string nick_by_me_;
	Gender gender_;
	buzz::Status::Show show_;//在线状态
	std::string status_;//签名
	std::string avatar_hash_;//头像哈希值
	Group * group_;
	void * ui_item_;
}

class Group
{
public:
	Group() :
		name_(""),
		ui_item_(NULL),
		type_(GROUP_TYPE_CUSTOMIZED)
	{
		reset_buddies();
	}
	~Group() {}
	enum GroupType { GROUP_TYPE_DEFAULT=0, GROUP_TYPE_CUSTOMIZED, GROUP_TYPE_BLACKLIST, GROUP_TYPE_STRANGER, GROUP_TYPE_RECOMMENDED, GROUP_TYPE_ONLINE, GROUP_TYPE_OFFLINE };
public:
	const std::string & name() const { return name_; }
	Buddy* get_buddy(const buzz::Jid & jid)
	{
		It4BuddiesByJid it = buddies_.find(jid);
		if (it != buddies_.end())
			return it->second;
		return NULL;
	}
public:
	void set_name(const std::string & name) { name_ = name; }
	void clear_buddies() 
	{ 
		if (!buddies_.empty())
			buddies_.clear(); 
	}
	void add_buddy(const Buddy* buddy)
	{
		if (buddy) {
			buddies_[buddy->jid()] = buddy;
			buddy->set_group(this);
		}
	}
	void remove_buddy(const buzz::Jid & jid)
	{
		It4BuddiesByJid it = buddies_.find(jid);
		if (it != buddies_.end()) {
			Buddy * buddy = it->second;
			buddy->set_group(NULL);
			buddies_.erase(it);
		}
	}
private:
	std::string name_;
	void * ui_item_;
	GroupType type_;
	BuddiesByJid buddies_;
};

class Account
{
private:
	Buddy myself_;
	std::string private_path_;
};

#endif //__DEFINE_HXX__
