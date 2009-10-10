/*
* Softcam plugin to VDR (C++)
*
* This code is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This code is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
* Or, point your browser to http://www.gnu.org/copyleft/gpl.html
*/

// initialy written by anetwolf anetwolf@hotmail.com
// based on crypto & protocol information from _silencer
// EMM & cmd 05 handling based on contribution from appiemulder

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <openssl/sha.h>

#include "cc.h"
#include "parse.h"
#include "helper.h"
#include "version.h"

static const char *cccamstr="CCcam";

// -- cCCcamCrypt --------------------------------------------------------------

class cCCcamCrypt {
private:
  unsigned char keytable[256];
  unsigned char state, counter, sum;
  //
  static void Swap(unsigned char *p1, unsigned char *p2);
public:
  void Init(const unsigned char *key, int length);
  void Decrypt(const unsigned char *in, unsigned char *out, int length);
  void Encrypt(const unsigned char *in, unsigned char *out, int length);
  static void ScrambleDcw(unsigned char *data, const unsigned char *nodeid, unsigned int shareid);
  static bool DcwChecksum(const unsigned char *data);
  static bool CheckConnectChecksum(const unsigned char *data, int length);
  static void Xor(unsigned char *data, int length);
  };

void cCCcamCrypt::Init(const unsigned char *key, int length)
{
  for(int pos=0; pos<=255; pos++) keytable[pos]=pos;
  int result=0;
  unsigned char curr_char=0;
  for(int pos=0; pos<=255; pos++) {
    curr_char+=keytable[pos] + key[result];
    Swap(keytable+pos,keytable+curr_char);
    result=(result+1)%length;
    }
  state=key[0]; counter=0; sum=0;
}

void cCCcamCrypt::Decrypt(const unsigned char *in, unsigned char *out, int length)
{
  for(int pos=0; pos<length; pos++) {
    sum+=keytable[++counter];
    Swap(keytable+counter,keytable+sum);
    out[pos]=in[pos] ^ state ^ keytable[(keytable[sum]+keytable[counter])&0xFF];
    state^=out[pos];
    }
}

void cCCcamCrypt::Encrypt(const unsigned char *in, unsigned char *out, int length)
{
  // There is a side-effect in this function:
  // If in & out pointer are the same, then state is xor'ed with modified input
  // (because output(=in ptr) is written before state xor)
  // This side-effect is used when initialising the encrypt state!
  for(int pos=0; pos<length; pos++) {
    sum+=keytable[++counter];
    Swap(keytable+counter,keytable+sum);
    out[pos]=in[pos] ^ state ^ keytable[(keytable[sum]+keytable[counter])&0xFF];
    state^=in[pos];
    }
}

void cCCcamCrypt::Swap(unsigned char *p1, unsigned char *p2)
{
  unsigned char tmp=*p1; *p1=*p2; *p2=tmp;
}

void cCCcamCrypt::ScrambleDcw(unsigned char *data, const unsigned char *nodeid, unsigned int shareid)
{
  long long n=bswap_64(*(long long *)nodeid);
  for(unsigned int i=0; i<16; i++) {
    char final=data[i]^(n&0xFF);
    if(i&1) final=~final;
    data[i]=final^(shareid&0xFF);
    n>>=4; shareid>>=2;
    }
}

bool cCCcamCrypt::DcwChecksum(const unsigned char *data)
{
  bool res=true;
  if(((data[0]+data[1]+data[2])&0xff)!=data[3] ||
     ((data[4]+data[5]+data[6])&0xff)!=data[7]) {
    res=false;
    PRINTF(L_CC_CCCAM2EX,"warning: even CW checksum failed");
    }
  if(((data[8]+data[9]+data[10])&0xff)!=data[11] ||
     ((data[12]+data[13]+data[14])&0xff)!=data[15]) {
    res=false;
    PRINTF(L_CC_CCCAM2EX,"warning: odd CW checksum failed");
    }
  return res;
}

bool cCCcamCrypt::CheckConnectChecksum(const unsigned char *data, int length)
{
  if(length==16) {
    bool valid=true;
    // CCcam 2.1.2 don't use connect checksum
    //for(int i=0; i<4; i++)
    //  if(((data[i+0]+data[i+4]+data[i+8])&0xFF)!=data[i+12]) valid=false;
    return valid;
    }
  return false;
}

void cCCcamCrypt::Xor(unsigned char *data, int length)
{
  if(length>=16)
    for(int index=0; index<8; index++) {
      data[8]=index*data[0];
      if(index<=5) data[0]^=cccamstr[index];
      data++;
      }
}

// -- cEcmShare ----------------------------------------------------------------

class cEcmShares;

class cEcmShare : public cSimpleItem {
friend class cEcmShares;
private:
  int source, transponder, pid;
  int shareid;
  int status;
public:
  cEcmShare(const cEcmInfo *ecm, int id);
  };

cEcmShare::cEcmShare(const cEcmInfo *ecm, int id)
{
  pid=ecm->ecm_pid;
  source=ecm->source;
  transponder=ecm->transponder;
  shareid=id;
  status=0;
}

// -- cEcmShares ---------------------------------------------------------------

class cEcmShares : public cSimpleList<cEcmShare> {
private:
  cEcmShare *Find(const cEcmInfo *ecm, int id);
public:
  int FindStatus(const cEcmInfo *ecm, int id);
  void AddStatus(const cEcmInfo *ecm, int id, int status);
  };

static cEcmShares ecmshares;

cEcmShare *cEcmShares::Find(const cEcmInfo *ecm, int id)
{
  for(cEcmShare *e=First(); e; e=Next(e))
    if(e->shareid==id && e->pid==ecm->ecm_pid && e->source==ecm->source && e->transponder==ecm->transponder)
      return e;
  return 0;
}

int cEcmShares::FindStatus(const cEcmInfo *ecm, int id)
{
  cEcmShare *e=Find(ecm,id);
  if(e) {
    PRINTF(L_CC_CCCAM2SH,"shareid %08x for %04x/%x/%x status %d",e->shareid,ecm->ecm_pid,ecm->source,ecm->transponder,e->status);
    return e->status;
    }
  return 0;
}

void cEcmShares::AddStatus(const cEcmInfo *ecm, int id, int status)
{
  cEcmShare *e=Find(ecm,id);
  const char *t="updated";
  if(!e) {
    Add((e=new cEcmShare(ecm,id)));
    t="added";
    }
  PRINTF(L_CC_CCCAM2SH,"%s shareid %08x for %04x/%x/%x status %d",t,id,ecm->ecm_pid,ecm->source,ecm->transponder,status);
  e->status=status;
}

// -- cShareProv ---------------------------------------------------------------

class cShareProv : public cSimpleItem {
public:
  int provid;
  };

// -- cShare -------------------------------------------------------------------

#define STDLAG 1000
#define MAXLAG 5000

class cShares;

class cShare : public cSimpleItem, public cIdSet {
friend class cShares;
private:
  int shareid, caid;
  cSimpleList<cShareProv> prov;
  int hops, lag;
  int status;
  unsigned char ua[8];
  bool emmready;
  cMsgCache *cache;
  //
  void Init(void);
  void SetCache(void);
public:
  cShare(int Shareid, int Caid, int Hops, const unsigned char *Ua);
  cShare(const cShare *s);
  ~cShare();
  bool UsesProv(void) const;
  bool HasProv(int provid) const;
  bool CheckAddProv(const unsigned char *prov, const unsigned char *sa);
  bool Compare(const cShare *s) const;
  int ShareID(void) const { return shareid; };
  int CaID(void) const { return caid;};
  int Hops(void) const { return hops; };
  int Lag(void) const { return lag; };
  int Status(void) const { return status; }
  bool EmmReady(void) const { return emmready; }
  cMsgCache *Cache(void) const { return cache; }
  };

cShare::cShare(int Shareid, int Caid, int Hops, const unsigned char *Ua)
{
  shareid=Shareid;
  caid=Caid;
  hops=Hops;
  lag=STDLAG; status=0;
  memcpy(ua,Ua,sizeof(ua));
  Init();
}

cShare::cShare(const cShare *s)
{
  shareid=s->shareid;
  caid=s->caid;
  hops=s->hops;
  lag=s->lag;
  status=s->status;
  memset(ua,0,sizeof(ua));
  Init();
}

cShare::~cShare()
{
  delete cache;
}

void cShare::Init(void)
{
  cache=0; emmready=false;
  if(!CheckNull(ua,sizeof(ua)))
    switch(caid>>8) {
      case 0x17:
      case 0x06: SetCard(new cCardIrdeto(ua[4],&ua[5])); emmready=true; break;
      case 0x01: SetCard(new cCardSeca(&ua[2])); emmready=true; break;
      case 0x0b: SetCard(new cCardConax(&ua[1])); emmready=true; break;
      case 0x09: SetCard(new cCardNDS(&ua[4])); emmready=true; break;
      case 0x05: SetCard(new cCardViaccess(&ua[3])); emmready=true; break;
      case 0x0d: SetCard(new cCardCryptoworks(&ua[3])); emmready=true; break;
      case 0x12:
      case 0x18: if((caid>>8)==0x18 || caid==0x1234) {
                   SetCard(new cCardNagra2(&ua[4]));
                   emmready=true;
                   break;
                   }
      // fall through
      default:   PRINTF(L_CC_CCCAM2,"share %08x: can't handle unique updates for CAID %04x",shareid,caid);
                 break;
      }
  SetCache();
}

void cShare::SetCache(void)
{
  if(emmready) {
    if(!cache) cache=new cMsgCache(32,0);
    if(!cache) {
      emmready=false;
      PRINTF(L_CC_CCCAM2,"share %08x: failed to alloc EMM cache",shareid);
      }
    }
  else {
    delete cache; cache=0;
    }
}

bool cShare::UsesProv(void) const
{
  switch(caid>>8) {
    case 0x01:
    case 0x05:
      return true;
    default:
      return false;
    }
}

bool cShare::HasProv(int provid) const
{
  for(cShareProv *sp=prov.First(); sp; sp=prov.Next(sp))
    if(sp->provid==provid) return true;
  return false;
}

bool cShare::CheckAddProv(const unsigned char *pr, const unsigned char *sa)
{
  unsigned char s[8];
  memset(&s[0],0,4); memcpy(&s[4],sa,4);
  int provid=UINT32_BE(pr-1)&0xFFFFFF;
  bool res=false;
  if(!CheckNull(s,sizeof(s)))
    switch(caid>>8) {
      case 0x17:
      case 0x06: AddProv(new cProviderIrdeto(s[4],&s[5])); res=true; break;
      case 0x01: AddProv(new cProviderSeca(&pr[1],&s[4])); res=true; break;
      case 0x0b: AddProv(new cProviderConax(&s[1])); res=true; break;
      case 0x09: AddProv(new cProviderNDS(&s[4])); res=true; break;
      case 0x05: AddProv(new cProviderViaccess(&pr[0],&s[4])); res=true; break;
      case 0x0d: AddProv(new cProviderCryptoworks(&s[3])); res=true; break;
      default:   PRINTF(L_CC_CCCAM2,"share %08x: can't handle shared updates for CAID %04x",shareid,caid);
                 break;
    }
  if(res) emmready=true;
  SetCache();

  if(UsesProv() && !HasProv(provid)) {
    cShareProv *sp=new cShareProv;
    sp->provid=provid;
    prov.Add(sp);
    res=true;
    }
  return res;
}

bool cShare::Compare(const cShare *s) const
{
  // success or untried is better ;)
  if((s->status<0)!=(status<0)) return s->status>=0;
  // lower lag is better
  if(s->lag!=lag) return s->lag<lag;
  // lower hops is better
  return s->hops<hops;
}

// -- cShares ------------------------------------------------------------------

class cShares : public cRwLock, public cSimpleList<cShare> {
private:
  cShare *Find(int shareid);
public:
  int GetShares(const cEcmInfo *ecm, cShares *ss);
  void SetLag(int shareid, int lag);
  bool HasCaid(int caid);
  };

cShare *cShares::Find(int shareid)
{
  for(cShare *s=First(); s; s=Next(s))
    if(s->shareid==shareid) return s;
  return 0;
}

void cShares::SetLag(int shareid, int lag)
{
  Lock(false);
  cShare *s=Find(shareid);
  if(s) {
    lag=min(lag,MAXLAG);
    if(s->lag==STDLAG) s->lag=(STDLAG+lag)/2;
    else s->lag=(3*s->lag+lag)/4;
    }
  Unlock();
}

bool cShares::HasCaid(int caid)
{
  for(cShare *s=First(); s; s=Next(s))
    if(s->CaID()==caid) return true;
  return false;
}

int cShares::GetShares(const cEcmInfo *ecm, cShares *ss)
{
  Lock(true);
  Clear();
  ss->Lock(false);
  for(cShare *s=ss->First(); s; s=ss->Next(s)) {
    if(s->caid==ecm->caId && (!s->UsesProv() || s->HasProv(ecm->provId)) && !Find(s->shareid)) {
      cShare *n=new cShare(s);
      n->status=ecmshares.FindStatus(ecm,n->shareid);
      // keep the list sorted
      cShare *l=0;
      for(cShare *t=First(); t; t=Next(t)) {
        if(t->Compare(n)) {
          if(l) Add(n,l); else Ins(n);
          n=0; break;
          }
        l=t;
        }
      if(n) Add(n);
      }
    }
  ss->Unlock();
  Unlock();
  return Count();
}

// -- CCcam protocol structs ---------------------------------------------------

struct CmdHeader {
  unsigned char flags, cmd;
  unsigned short cmdlen; // BE
  } __attribute__((packed));

#define SETCMDLEN(h,l) BYTE2_BE(&(h)->cmdlen,(l)-4)
#define CMDLEN(h)      (UINT16_BE(&(h)->cmdlen)+4)

struct ClientInfo {
  struct CmdHeader header;
  char username[20];
  unsigned char nodeid[8];
  unsigned char wantemus;
  char version[32], build[32];
  } __attribute__((packed));

struct ServerInfo {
  struct CmdHeader header;
  unsigned char nodeid[8];
  char version[32], build[32];
  } __attribute__((packed));

struct EcmRequest {
  struct CmdHeader header;
  unsigned short caid;  // BE
  unsigned int provid;  // BE
  unsigned int shareid; // BE
  unsigned short sid;   // BE
  unsigned char datalen;
  unsigned char data[0];
  } __attribute__((packed));

struct EmmRequest {
  struct CmdHeader header;
  unsigned short caid;  // BE
  unsigned char dummy;  //XXX what is that?
  unsigned int provid;  // BE
  unsigned int shareid; // BE
  unsigned char datalen;
  unsigned char data[0];
  } __attribute__((packed));

struct DcwAnswer {
  struct CmdHeader header;
  unsigned char cw[16];
  } __attribute__((packed));

struct DelShare {
  struct CmdHeader header;
  unsigned int shareid;  // BE
  } __attribute__((packed));

struct AddShare {
  struct CmdHeader header;
  unsigned int shareid;  // BE
  unsigned int remoteid; // BE
  unsigned short caid;   // BE
  unsigned char uphops, maxdown;
  unsigned char cardserial[8];
  } __attribute__((packed));

struct ProvInfo {
  unsigned char count;
  struct {
    unsigned char provid[3];
    unsigned char sa[4];
    } prov[0];
  } __attribute__((packed));

struct NodeInfo {
  unsigned char count;
  unsigned char nodeid[0][8];
  } __attribute__((packed));

// -- cCardClientCCcam2 ---------------------------------------------------------

#define MAX_ECM_TIME 3000     // ms
#define PING_TIME    120*1000 // ms

class cCardClientCCcam2 : public cCardClient , private cThread {
private:
  cCCcamCrypt encr, decr;
  cShares shares;
  unsigned char nodeid[8];
  int shareid;
  char username[21], password[64], versstr[32], buildstr[32];
  bool login, emmProcessing;
  cTimeMs lastsend;
  int pendingDCW, keymaskpos;
  //
  bool newcw;
  unsigned char cw[16];
  cMutex cwmutex;
  cCondVar cwwait;
  tThreadId readerTid;
  //
  void PacketAnalyzer(const struct CmdHeader *hdr, int length);
  int CryptRecv(unsigned char *data, int len, int to=-1);
  bool CryptSend(const unsigned char *data, int len);
protected:
  virtual bool Login(void);
  virtual void Logout(void);
  virtual void Action(void);
public:
  cCardClientCCcam2(const char *Name);
  ~cCardClientCCcam2();
  virtual bool Init(const char *CfgDir);
  virtual bool CanHandle(unsigned short SysId);
  virtual bool ProcessECM(const cEcmInfo *ecm, const unsigned char *data, unsigned char *Cw, int cardnum);
  virtual bool ProcessEMM(int caSys, const unsigned char *data);
  };

static cCardClientLinkReg<cCardClientCCcam2> __ncd("cccam2");

cCardClientCCcam2::cCardClientCCcam2(const char *Name)
:cCardClient(Name)
,cThread("CCcam2 reader")
{
  shareid=0; readerTid=0; pendingDCW=keymaskpos=0; newcw=login=emmProcessing=false;
  so.SetRWTimeout(10*1000);
}

cCardClientCCcam2::~cCardClientCCcam2()
{
  Logout();
}

void cCardClientCCcam2::PacketAnalyzer(const struct CmdHeader *hdr, int length)
{
  if(CMDLEN(hdr)<=length) {
    switch(hdr->cmd) {
      case 0:
        break;
      case 1:
        {
        struct DcwAnswer *dcw=(struct DcwAnswer *)hdr;
        if(pendingDCW>0) pendingDCW--;
        PRINTF(L_CC_CCCAM2,"got CW, current shareid %08x, pending %d",shareid,pendingDCW);
        unsigned char tempcw[16];
        memcpy(tempcw,dcw->cw,16);
        LDUMP(L_CC_CCCAM2DT,tempcw,16,"scrambled    CW");
        cCCcamCrypt::ScrambleDcw(tempcw,nodeid,shareid);
        LDUMP(L_CC_CCCAM2DT,tempcw,16,"un-scrambled CW");
        if(cCCcamCrypt::DcwChecksum(tempcw) || pendingDCW==0) {
          cwmutex.Lock();
          newcw=true;
          memcpy(cw,tempcw,16);
          cwwait.Broadcast();
          cwmutex.Unlock();
          }
        else PRINTF(L_CC_CCCAM2,"pending DCW, skipping bad CW");
        decr.Decrypt(tempcw,tempcw,16);
        if(keymaskpos>0 && --keymaskpos<=2) {
          PRINTF(L_CC_CCCAM2,"disconnecting due to key limit...");
          Logout();
          }
        break;
        }
      case 2:
        PRINTF(L_CC_CCCAM2EX,"EMM ack");
        break;
      case 4:
        {
        struct DelShare *del=(struct DelShare *)hdr;
        int shareid=UINT32_BE(&del->shareid);
        bool check=false;
        shares.Lock(true);
        for(cShare *s=shares.First(); s;) {
          cShare *n=shares.Next(s);
          if(s->ShareID()==shareid) {
            PRINTF(L_CC_CCCAM2SH,"REMOVE share %08x caid: %04x (count %d)",s->ShareID(),s->CaID(),shares.Count());
            int caid=s->CaID();
            if(s->EmmReady()) check=true;
            shares.Del(s);
            if(!shares.HasCaid(caid)) CaidsChanged();
            }
          s=n;
          }
        if(check && emmProcessing) {
          bool emm=false;
          for(cShare *s=shares.First(); s; s=shares.Next(s))
            if(s->EmmReady()) { emm=true; break; }
          if(!emm) {
            emmProcessing=false;
            PRINTF(L_CC_CCCAM2,"disabled EMM processing");
            }
          }
        shares.Unlock();
        break;
        }
      case 5:
        {
        static const struct CmdHeader resp = { 0,5,0 };
        if(CryptSend((unsigned char *)&resp,sizeof(resp))<0)
          PRINTF(L_CC_CCCAM2,"failed to send cmd 05 response");
        keymaskpos=60;
        break;
        }
      case 6:
        PRINTF(L_CC_CCCAM2,"server PONG");
        break;
      case 7:
        {
        struct AddShare *add=(struct AddShare *)hdr;
        int caid=UINT16_BE(&add->caid);
        int shareid=UINT32_BE(&add->shareid);
        shares.Lock(false);
        if(!shares.HasCaid(caid)) CaidsChanged();
        shares.Unlock();
        cShare *s=new cShare(shareid,caid,add->uphops,add->cardserial);
        LBSTARTF(L_CC_CCCAM2SH);
        LBPUT("ADD share %08x hops %d maxdown %d caid %04x ua ",shareid,add->uphops,add->maxdown,caid);
        for(int i=0; i<8; i++) LBPUT("%02x",add->cardserial[i]);
        struct ProvInfo *prov=(struct ProvInfo *)(add+1);
        if(prov->count>0) {
          bool first=true;
          for(int i=0; i<prov->count; i++) {
            if(s->CheckAddProv(prov->prov[i].provid,prov->prov[i].sa)) {
              if(first) { LBPUT(" prov"); first=false; }
              LBPUT(" %06x/%08x",UINT32_BE(prov->prov[i].provid-1)&0xFFFFFF,UINT32_BE(prov->prov[i].sa));
              }
            }
          }
        if(s->EmmReady()) LBPUT(" (EMM)");
        LBEND();
        shares.Lock(true);
        shares.Add(s);
        if(s->EmmReady() && !emmProcessing && emmAllowed) {
          emmProcessing=true;
          PRINTF(L_CC_CCCAM2,"enabled EMM processing");
          }
        shares.Unlock();
        break;
        }
      case 8:
        {
        struct ServerInfo *srv=(struct ServerInfo *)hdr;
        LDUMP(L_CC_LOGIN,srv->nodeid,sizeof(srv->nodeid),"%s: server version %s build %s nodeid",name,srv->version,srv->build);
        break;
        }
      case 0xff:
      case 0xfe:
        if(pendingDCW>0) pendingDCW--;
        if(hdr->cmd==0xfe) PRINTF(L_CC_CCCAM2,"share not found on server (%d pending)",pendingDCW);
        else PRINTF(L_CC_CCCAM2,"server can't decode this ecm, (%d pending)",pendingDCW);
        cwmutex.Lock();
        newcw=false;
        cwwait.Broadcast();
        cwmutex.Unlock();
        break;
      default:
        LDUMP(L_CC_CCCAM2,hdr,length,"got unhandled cmd %x:",hdr->cmd);
        break;
      }
    }
  else
    PRINTF(L_CC_CCCAM2,"cmdlen mismatch: cmdlen=%d length=%d",CMDLEN(hdr),length);
}

bool cCardClientCCcam2::CanHandle(unsigned short SysId)
{
  if(!login) return cCardClient::CanHandle(SysId);
  bool res=false;
  shares.Lock(false);
  for(cShare *s=shares.First(); s; s=shares.Next(s))
    if(s->CaID()==SysId) { res=true; break; }
  shares.Unlock();
  return res;
}

bool cCardClientCCcam2::Init(const char *config)
{
  cMutexLock lock(this);
  strn0cpy(versstr,"2.0.11",sizeof(versstr));
  strn0cpy(buildstr,"2892",sizeof(buildstr));
  int n=0, num=0;
  Logout();
  char ni[17];
  if(!ParseStdConfig(config,&num)
     || (n=sscanf(&config[num],":%20[^:]:%63[^:]:%16[^:]:%31[^/]:%31[^:]",username,password,ni,versstr,buildstr))<2 ) return false;
  PRINTF(L_CC_CORE,"%s: username=%s password=%s",name,username,password);
  if(n>2 && strcmp(ni,"*")) {
    const char *tmp=ni;
    if(GetHex(tmp,nodeid,sizeof(nodeid),false)!=sizeof(nodeid)) return false;
    }
  else
    for(unsigned int i=0; i<sizeof(nodeid); i++) nodeid[i]=rand();
  LDUMP(L_CC_CORE,nodeid,sizeof(nodeid),"Our nodeid:");
  PRINTF(L_CC_CORE,"Pretended CCcam version '%s' build '%s'",versstr,buildstr);
  return Immediate() ? Login() : true;
}

void cCardClientCCcam2::Logout(void)
{
  login=false;
  Cancel(cThread::ThreadId()!=readerTid ? 2:-1);
  readerTid=0;
  cCardClient::Logout();
  shares.Lock(true);
  shares.Clear();
  emmProcessing=false;
  shares.Unlock();
  pendingDCW=keymaskpos=0;
}

bool cCardClientCCcam2::Login(void)
{
  Logout();
  if(!so.Connect(hostname,port)) return false;
  so.SetQuietLog(true);

  unsigned char buffer[512];
  int len;
  if((len=RecvMsg(buffer,16))<0) {
    PRINTF(L_CC_CCCAM2,"no welcome from server");
    return false;
    }
  LDUMP(L_CC_CCCAM2DT,buffer,len,"welcome answer:");
  if(!cCCcamCrypt::CheckConnectChecksum(buffer,len)) {
    PRINTF(L_CC_CCCAM2,"bad welcome from server");
    Logout();
    return false;
    }
  PRINTF(L_CC_CCCAM2EX,"welcome checksum correct");

  cCCcamCrypt::Xor(buffer,len);
  unsigned char buff2[64];
  SHA1(buffer,len,buff2);
  decr.Init(buff2,20);
  decr.Decrypt(buffer,buffer,16);
  encr.Init(buffer,16);
  encr.Encrypt(buff2,buff2,20);

  LDUMP(L_CC_CCCAM2DT,buff2,20,"welcome response:");
  if(!CryptSend(buff2,20)) {
    PRINTF(L_CC_CCCAM2,"failed to send welcome response");
    return false;
    }

  memset(buffer,0,20);
  strcpy((char *)buffer,username);
  LDUMP(L_CC_CCCAM2DT,buffer,20,"send username:");
  if(!CryptSend(buffer,20)) {
    PRINTF(L_CC_CCCAM2,"failed to send username");
    return false;
    }

  encr.Encrypt((unsigned char *)password,buffer,strlen(password));
  if(!CryptSend((unsigned char *)cccamstr,6)) {
    PRINTF(L_CC_CCCAM2,"failed to send password hash");
    return false;
    }

  if((len=CryptRecv(buffer,20))<0) {
    PRINTF(L_CC_CCCAM2,"no login answer from server");
    return false;
    }
  LDUMP(L_CC_CCCAM2DT,buffer,len,"login answer:");

  if(strcmp(cccamstr,(char *)buffer)!=0) {
    PRINTF(L_CC_CCCAM2,"login failed");
    Logout();
    return false;
    }
  PRINTF(L_CC_LOGIN,"CCcam login succeed");

  struct ClientInfo clt;
  memset(&clt,0,sizeof(clt));
  clt.header.cmd=0;
  SETCMDLEN(&clt.header,sizeof(clt));
  strcpy(clt.username,username);
  strn0cpy(clt.version,versstr,sizeof(clt.version));
  strn0cpy(clt.build,buildstr,sizeof(clt.build));
  memcpy(clt.nodeid,nodeid,8);
  LDUMP(L_CC_CCCAM2DT,&clt,sizeof(clt),"send clientinfo:");
  if(!CryptSend((unsigned char*)&clt,sizeof(clt))) {
    PRINTF(L_CC_CCCAM2,"failed to send clientinfo");
    return false;
    }
  login=true;
  Start();
  return true;
}

bool cCardClientCCcam2::ProcessECM(const cEcmInfo *ecm, const unsigned char *data, unsigned char *Cw, int cardnum)
{
  cMutexLock lock(this);
  if(!so.Connected() && !Login()) { Logout(); return false; }
  if(!CanHandle(ecm->caId)) return false;
  PRINTF(L_CC_CCCAM2,"%d: ECM caid %04x prov %04x sid %d pid %04x",cardnum,ecm->caId,ecm->provId,ecm->prgId,ecm->ecm_pid);
  int sctlen=SCT_LEN(data);
  if(sctlen>=256) {
    PRINTF(L_CC_CCCAM2,"ECM data length >=256 not supported by CCcam");
    return false;
    }
  int ecm_len=sizeof(struct EcmRequest)+sctlen;
  struct EcmRequest *req=(struct EcmRequest *)AUTOMEM(ecm_len);
  memset(req,0,sizeof(struct EcmRequest));
  memcpy(req+1,data,sctlen);
  req->header.cmd=1;
  SETCMDLEN(&req->header,ecm_len);
  BYTE2_BE(&req->caid,ecm->caId);
  BYTE4_BE(&req->provid,ecm->provId);
  BYTE2_BE(&req->sid,ecm->prgId);
  req->datalen=sctlen;
  cShares curr;
  if(curr.GetShares(ecm,&shares)<1) {
    PRINTF(L_CC_CCCAM2,"no shares for this ECM");
    return false;
    }
  if(LOG(L_CC_CCCAM2SH)) {
    PRINTF(L_CC_CCCAM2SH,"share try list for caid %04x prov %06x pid %04x",ecm->caId,ecm->provId,ecm->ecm_pid);
    for(cShare *s=curr.First(); s; s=curr.Next(s))
      PRINTF(L_CC_CCCAM2SH,"shareid %08x hops %d %c lag %4d",s->ShareID(),s->Hops(),s->Status()>0?'+':(s->Status()<0?'-':' '),s->Lag());
    }
  cTimeMs max(MAX_ECM_TIME);
  for(cShare *s=curr.First(); s && !max.TimedOut(); s=curr.Next(s)) {
    if((shareid=s->ShareID())==0) continue;
    BYTE4_BE(&req->shareid,shareid);
    PRINTF(L_CC_CCCAM2EX,"now try shareid %08x",shareid);
    LDUMP(L_CC_CCCAM2DT,req,ecm_len,"send ecm:");
    if(pendingDCW>0)
      PRINTF(L_CC_CCCAM2,"WARN: there are pending %d DCW answers. This may cause trouble...",pendingDCW);
    pendingDCW++;
    if(!CryptSend((unsigned char *)req,ecm_len)) {
      PRINTF(L_CC_CCCAM2,"failed to send ecm request");
      break;
      }
    cwmutex.Lock();
    newcw=false;
    cTimeMs lag;
    if(cwwait.TimedWait(cwmutex,MAXLAG)) {
      uint64_t l=lag.Elapsed();
      shares.SetLag(shareid,l);
      PRINTF(L_CC_CCCAM2EX,"wait returned after %lld",l);
      if(newcw) {
        // check for partial CW
        if(!CheckNull(cw+0,8)) memcpy(Cw+0,cw+0,8);
        if(!CheckNull(cw+8,8)) memcpy(Cw+8,cw+8,8);
        cwmutex.Unlock();
        ecmshares.AddStatus(ecm,shareid,1);
        PRINTF(L_CC_CCCAM2,"got CW");
        return true;
        }
      else PRINTF(L_CC_CCCAM2EX,"no CW from this share");
      }
    else {
      uint64_t l=lag.Elapsed();
      shares.SetLag(shareid,l);
      PRINTF(L_CC_CCCAM2EX,"getting CW timed out after %lld",l);
      }
    ecmshares.AddStatus(ecm,shareid,-1);
    cwmutex.Unlock();
    }
  PRINTF(L_CC_ECM,"%s: unable to decode the channel",name);
  return false;
}

bool cCardClientCCcam2::ProcessEMM(int caSys, const unsigned char *data)
{
  bool res=false;
  if(emmProcessing && emmAllowed) {
    cMutexLock lock(this);
    shares.Lock(false);
    for(cShare *s=shares.First(); s; s=shares.Next(s)) {
      if(s->EmmReady() && s->CaID()==caSys) {
        cProvider *p;
        cAssembleData ad(data);
        if(s->MatchAndAssemble(&ad,0,&p)) {
          const unsigned char *d;
          while((d=ad.Assembled())) {
            int len=SCT_LEN(d);
            int id=s->Cache()->Get(d,len,0);
            if(id>0) {
              if(len<256) {
                unsigned char bb[sizeof(struct EmmRequest)+256];
                struct EmmRequest *req=(struct EmmRequest *)bb;
                int emm_len=sizeof(struct EmmRequest)+len;
                memset(req,0,sizeof(struct EmmRequest));
                memcpy(req+1,d,len);
                req->header.cmd=2;
                SETCMDLEN(&req->header,emm_len);
                BYTE2_BE(&req->caid,s->CaID());
                BYTE4_BE(&req->provid,p ? p->ProvId() : 0);
                BYTE4_BE(&req->shareid,s->ShareID());
                req->datalen=len;
                if(!CryptSend((unsigned char *)req,emm_len))
                  PRINTF(L_CC_CCCAM2,"failed to send emm request");
                }
              else PRINTF(L_CC_CCCAM2,"EMM data length >=256 not supported by CCcam");
              s->Cache()->Cache(id,true,0);
              }
            }
          res=true;
          }
        }
      }
    shares.Unlock();
    }
  return res;
}

void cCardClientCCcam2::Action(void)
{
  readerTid=cThread::ThreadId();
  int cnt=0;
  while(Running() && so.Connected()) {
    unsigned char recvbuff[1024];
    int len=sizeof(recvbuff)-cnt;
    if(len==0) {
      HEXDUMP(L_GEN_DEBUG,recvbuff,sizeof(recvbuff),"internal: cccam2 read buffer overflow");
      Logout();
      break;
      }
    len=CryptRecv(recvbuff+cnt,-len,200);
    if(len>0) {
      HEXDUMP(L_CC_CCCAM2DT,recvbuff+cnt,len,"net read: len=%d cnt=%d",len,cnt+len);
      cnt+=len;
      }
    int proc=0;
    while(proc+4<=cnt) {
      struct CmdHeader *hdr=(struct CmdHeader *)(recvbuff+proc);
      int l=CMDLEN(hdr);
      if(l>(int)sizeof(recvbuff))
        PRINTF(L_GEN_DEBUG,"internal: cccam2 cmd length exceed buffer size");
      if(proc+l>cnt) break;
      LDUMP(L_CC_CCCAM2DT,hdr,l,"msg in:");
      PacketAnalyzer(hdr,l);
      proc+=l;
      }
    if(proc) {
      cnt-=proc;
      memmove(recvbuff,recvbuff+proc,cnt);
      }
    if(lastsend.TimedOut()) {
      static const struct CmdHeader ping = { 0,6,0 };
      if(CryptSend((unsigned char *)&ping,sizeof(ping))<0)
        PRINTF(L_CC_CCCAM2,"failed to send server PING");
      }
    usleep(10);
    }
  readerTid=0;
}

int cCardClientCCcam2::CryptRecv(unsigned char *data, int len, int to)
{
  int r=RecvMsg(data,len,to);
  if(r>0) decr.Decrypt(data,data,r);
  return r;
}

bool cCardClientCCcam2::CryptSend(const unsigned char *data, int len)
{
  unsigned char *buff=AUTOMEM(len);
  encr.Encrypt(data,buff,len);
  lastsend.Set(PING_TIME);
  return SendMsg(buff,len);
}
