#pragma once

// A bounded, buffered Fetch subset for the native worker. All I/O and credential
// substitution remain in the parent broker; this script has no socket/OS access.
inline constexpr const char* networkBootstrap = R"JS(
(() => {
  const rpc=__rpc, cancelRpc=__cancelRpc;
  const unsupported=name=>{throw new TypeError(name+' is not implemented by the native sandbox');};
  const token=/^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
  const headerState=new WeakMap(), bodyState=new WeakMap(), requestState=new WeakMap(), responseState=new WeakMap(), signalState=new WeakMap();
  const headerName=name=>{name=String(name);if(!token.test(name))throw new TypeError('Invalid header name');return name.toLowerCase();};
  const headerValue=value=>{value=String(value);if(/[^\t\x20-\x7e\x80-\xff]/.test(value))throw new TypeError('Invalid header value');return value.replace(/^[\t ]+|[\t ]+$/g,'');};
  const headersOf=self=>{const s=headerState.get(self);if(!s)throw new TypeError('Invalid Headers receiver');return s;};
  class Headers {
    constructor(init={}) {
      headerState.set(this,{values:new Map(),immutable:false});
      if(init===null||typeof init!=='object')throw new TypeError('Invalid Headers initializer');
      if(typeof init[Symbol.iterator]==='function') {
        for(const pair of init){if(!pair||typeof pair[Symbol.iterator]!=='function')throw new TypeError('Invalid header pair');const p=Array.from(pair);if(p.length!==2)throw new TypeError('Invalid header pair');this.append(p[0],p[1]);}
      } else for(const key of Object.keys(init))this.append(key,init[key]);
    }
    append(name,value){name=headerName(name);value=headerValue(value);const s=headersOf(this);if(s.immutable)throw new TypeError('Response headers are immutable');s.values.set(name,s.values.has(name)?s.values.get(name)+', '+value:value);}
    set(name,value){name=headerName(name);value=headerValue(value);const s=headersOf(this);if(s.immutable)throw new TypeError('Response headers are immutable');s.values.set(name,value);}
    get(name){return headersOf(this).values.get(headerName(name))??null;}
    has(name){return headersOf(this).values.has(headerName(name));}
    delete(name){const s=headersOf(this);if(s.immutable)throw new TypeError('Response headers are immutable');s.values.delete(headerName(name));}
    *entries(){for(const key of [...headersOf(this).values.keys()].sort())yield [key,headersOf(this).values.get(key)];}
    *keys(){for(const [key] of this.entries())yield key;}
    *values(){for(const [,value] of this.entries())yield value;}
    forEach(callback,thisArg){if(typeof callback!=='function')throw new TypeError('Headers callback must be a function');for(const [key,value] of this.entries())callback.call(thisArg,value,key,this);}
    [Symbol.iterator](){return this.entries();}
    getSetCookie(){return [];}
  }
  class DOMException extends Error {
    constructor(message='',name='Error'){super(String(message));this.name=String(name);}
    get code(){return this.name==='AbortError'?20:this.name==='TimeoutError'?23:0;}
  }
  const abortError=()=>new DOMException('The operation was aborted.','AbortError');
  const signalOf=self=>{const s=signalState.get(self);if(!s)throw new TypeError('Invalid AbortSignal receiver');return s;};
  const signalKey={};
  class AbortSignal {
    constructor(key){if(key!==signalKey)throw new TypeError('Illegal AbortSignal constructor');signalState.set(this,{aborted:false,reason:undefined,listeners:new Map(),onabort:null,cleanup:[]});}
    get aborted(){return signalOf(this).aborted;}
    get reason(){return signalOf(this).reason;}
    get onabort(){return signalOf(this).onabort;}
    set onabort(value){signalOf(this).onabort=typeof value==='function'?value:null;}
    throwIfAborted(){if(this.aborted)throw this.reason;}
    addEventListener(type,callback,options={}){if(type!=='abort'||callback==null)return;if(typeof callback!=='function'&&typeof callback.handleEvent!=='function')throw new TypeError('Invalid event listener');const s=signalOf(this);if(!s.listeners.has(callback))s.listeners.set(callback,!!(options&&options.once));}
    removeEventListener(type,callback){if(type==='abort')signalOf(this).listeners.delete(callback);}
    static abort(reason){const signal=new AbortSignal(signalKey);abort(signal,reason);return signal;}
    static timeout(ms){if(!Number.isSafeInteger(ms)||ms<0||ms>180000)throw new RangeError('AbortSignal timeout must be 0..180000 ms');const signal=new AbortSignal(signalKey);const timer=setTimeout(()=>abort(signal,new DOMException('The operation timed out.','TimeoutError')),ms);signalOf(signal).cleanup.push(()=>clearTimeout(timer));return signal;}
    static any(signals){const list=Array.from(signals);for(const signal of list)signalOf(signal);const combined=new AbortSignal(signalKey);for(const signal of list){if(signal.aborted){abort(combined,signal.reason);break;}const listener=()=>abort(combined,signal.reason);signal.addEventListener('abort',listener,{once:true});signalOf(combined).cleanup.push(()=>signal.removeEventListener('abort',listener));}return combined;}
  }
  function abort(signal,reason){const s=signalOf(signal);if(s.aborted)return;s.aborted=true;s.reason=reason===undefined?abortError():reason;for(const cleanup of s.cleanup.splice(0))cleanup();const event=Object.freeze({type:'abort',target:signal,currentTarget:signal});const listeners=[...s.listeners];s.listeners.clear();if(s.onabort)listeners.push([s.onabort,true]);for(const [callback,once] of listeners){if(!once)s.listeners.set(callback,false);try{if(typeof callback==='function')callback.call(signal,event);else callback.handleEvent(event);}catch(error){console.error('Abort listener error',String(error));}}}
  class AbortController {
    constructor(){Object.defineProperty(this,'signal',{value:new AbortSignal(signalKey),enumerable:true});}
    abort(reason){abort(this.signal,reason);}
  }
  function bodyData(value){
    if(value===undefined||value===null)return {bytes:null,text:null,used:false,signal:null};
    if(typeof value==='string')return {bytes:Buffer.from(value),text:value,used:false,signal:null};
    if(value instanceof ArrayBuffer)return {bytes:Buffer.from(new Uint8Array(value)),text:null,used:false,signal:null};
    if(ArrayBuffer.isView(value))return {bytes:Buffer.from(new Uint8Array(value.buffer,value.byteOffset,value.byteLength)),text:null,used:false,signal:null};
    return unsupported('Fetch body type (use string, ArrayBuffer or typed array)');
  }
  const bodyOf=self=>{const s=bodyState.get(self);if(!s)throw new TypeError('Invalid body receiver');return s;};
  const copyBody=s=>({bytes:s.bytes===null?null:Buffer.from(s.bytes),text:s.text,used:false,signal:s.signal});
  function consume(self){const s=bodyOf(self);if(s.signal)s.signal.throwIfAborted();if(s.used)throw new TypeError('Body has already been consumed');if(s.bytes!==null)s.used=true;return s.bytes===null?new Uint8Array():new Uint8Array(s.bytes);}
  const bodyMethods={
    get bodyUsed(){return bodyOf(this).used;},
    get body(){if(bodyOf(this).bytes===null)return null;return unsupported('ReadableStream body');},
    async arrayBuffer(){return consume(this).buffer;},
    async bytes(){return consume(this);},
    async text(){return __decodeUtf8(consume(this)).replace(/^\uFEFF/,'');},
    async json(){return JSON.parse(await this.text());},
    async blob(){return unsupported('Blob');},
    async formData(){return unsupported('FormData');}
  };
  function addBody(prototype){Object.defineProperties(prototype,Object.getOwnPropertyDescriptors(bodyMethods));}
  class Request {
    constructor(input,init={}){
      if(init===null||typeof init!=='object')throw new TypeError('Invalid Request options');
      for(const key of Object.keys(init))if(!['method','headers','body','redirect','signal','credentials'].includes(key))unsupported('Request option '+key);
      if(init.credentials!==undefined&&init.credentials!=='omit')unsupported('Request credentials other than omit');
      const previous=requestState.get(input), inherited=previous?bodyOf(input):null;
      const url=previous?previous.url:String(input);
      if(!/^https?:\/\//i.test(url))throw new TypeError('Fetch requires an absolute HTTP(S) URL');
      let method=String(init.method??previous?.method??'GET');
      if(!token.test(method)||['CONNECT','TRACE','TRACK'].includes(method.toUpperCase()))throw new TypeError('Invalid fetch method');
      if(['DELETE','GET','HEAD','OPTIONS','POST','PUT'].includes(method.toUpperCase()))method=method.toUpperCase();
      const headers=new Headers(init.headers??previous?.headers??{}), redirect=init.redirect??previous?.redirect??'follow';
      if(!['follow','manual','error'].includes(redirect))throw new TypeError('Invalid redirect mode');
      const signal=init.signal??previous?.signal??new AbortSignal(signalKey);signalOf(signal);
      const hasBody=Object.prototype.hasOwnProperty.call(init,'body');
      if(!hasBody&&inherited?.used)throw new TypeError('Request body has already been consumed');
      const body=hasBody?bodyData(init.body):inherited?copyBody(inherited):bodyData(null);
      if((method==='GET'||method==='HEAD')&&body.bytes!==null)throw new TypeError('GET and HEAD requests cannot have a body');
      if(body.bytes!==null&&body.bytes.byteLength>1048576)throw new RangeError('Fetch request body exceeds 1 MiB');
      if(body.text!==null&&!headers.has('content-type'))headers.set('content-type','text/plain;charset=UTF-8');
      if(!hasBody&&inherited?.bytes!==null&&inherited)inherited.used=true;
      body.signal=signal;bodyState.set(this,body);requestState.set(this,{url,method,headers,redirect,signal});
    }
    get url(){return requestState.get(this).url;}get method(){return requestState.get(this).method;}get headers(){return requestState.get(this).headers;}get redirect(){return requestState.get(this).redirect;}get signal(){return requestState.get(this).signal;}get credentials(){return 'omit';}
    clone(){const body=bodyOf(this);if(body.used)throw new TypeError('Request body has already been consumed');const clone=Object.create(Request.prototype);bodyState.set(clone,copyBody(body));requestState.set(clone,{...requestState.get(this),headers:new Headers(this.headers)});return clone;}
  }
  addBody(Request.prototype);
  class Response {
    constructor(body=null,init={}){
      if(init===null||typeof init!=='object')throw new TypeError('Invalid Response options');
      const status=init.status??200;
      if(!Number.isInteger(status)||status<200||status>599)throw new RangeError('Response status must be 200..599');
      const statusText=String(init.statusText??'');if(/[^\t\x20-\x7e\x80-\xff]/.test(statusText))throw new TypeError('Invalid response status text');
      const data=bodyData(body);if([204,205,304].includes(status)&&data.bytes!==null)throw new TypeError('This response status cannot have a body');
      const headers=new Headers(init.headers??{});if(data.text!==null&&!headers.has('content-type'))headers.set('content-type','text/plain;charset=UTF-8');
      bodyState.set(this,data);responseState.set(this,{status,statusText,headers,url:'',redirected:false,type:'default'});
    }
    get status(){return responseState.get(this).status;}get statusText(){return responseState.get(this).statusText;}get ok(){return this.status>=200&&this.status<=299;}get headers(){return responseState.get(this).headers;}get url(){return responseState.get(this).url;}get redirected(){return responseState.get(this).redirected;}get type(){return responseState.get(this).type;}
    clone(){const body=bodyOf(this);if(body.used)throw new TypeError('Response body has already been consumed');const clone=Object.create(Response.prototype), state=responseState.get(this),headers=new Headers(state.headers);headersOf(headers).immutable=headersOf(state.headers).immutable;bodyState.set(clone,copyBody(body));responseState.set(clone,{...state,headers});return clone;}
    static json(value,init={}){const text=JSON.stringify(value);if(text===undefined)throw new TypeError('Response.json value is not serializable');const headers=new Headers(init.headers??{});if(!headers.has('content-type'))headers.set('content-type','application/json');return new Response(text,{...init,headers});}
    static error(){const result=new Response();Object.assign(responseState.get(result),{status:0,type:'error'});headersOf(result.headers).immutable=true;return result;}
    static redirect(url,status=302){if(![301,302,303,307,308].includes(status))throw new RangeError('Invalid redirect status');if(!/^https?:\/\//i.test(String(url)))throw new TypeError('Redirect requires an absolute HTTP(S) URL');const result=new Response(null,{status,headers:{location:String(url)}});headersOf(result.headers).immutable=true;return result;}
  }
  addBody(Response.prototype);
  async function fetch(input,init){
    const request=new Request(input,init), body=bodyOf(request), signal=request.signal;
    signal.throwIfAborted();
    const args={url:request.url,method:request.method,headers:[...request.headers],redirect:request.redirect};
    if(body.bytes!==null){body.used=true;if(body.text!==null)args.body=body.text;else args.bodyBase64=Buffer.from(body.bytes).toString('base64');}
    const pending=rpc('network',args), listener=()=>cancelRpc(pending.nativeRequestId);
    signal.addEventListener('abort',listener,{once:true});
    try {
      const wire=await pending;signal.throwIfAborted();
      const result=Object.create(Response.prototype),headers=new Headers(wire.headers??[]);headersOf(headers).immutable=true;
      const data=bodyData(Buffer.from(wire.bodyBase64??'','base64'));data.signal=signal;
      if(request.method==='HEAD'||[204,205,304].includes(wire.status))data.bytes=null;
      bodyState.set(result,data);responseState.set(result,{status:wire.status,statusText:wire.statusText,headers,url:wire.url,redirected:!!wire.redirected,type:'basic'});
      return result;
    } catch(error){if(signal.aborted)throw signal.reason;throw error;}
    finally {signal.removeEventListener('abort',listener);}
  }
  Object.assign(globalThis,{Headers,Request,Response,AbortController,AbortSignal,DOMException,fetch});
})();
)JS";
