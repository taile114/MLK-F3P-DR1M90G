// Fixed-shape CPU execution plan. Buffers and callbacks are built once.
#pragma once
#include <functional>
#include <arm_neon.h>

namespace { namespace fused {
struct Value { float* data; Shape shape; };
std::size_t size(const Value& v) { return elements(v.shape); }
float dot(const float* x, const float* w, unsigned count, bool neon) {
    float sum = 0;
    unsigned i = 0;
    if (neon) {
        auto lanes = vdupq_n_f32(0);
        for (; i + 4 <= count; i += 4) lanes = vaddq_f32(lanes, vmulq_f32(vld1q_f32(x+i), vld1q_f32(w+i)));
        sum = vaddvq_f32(lanes);
    }
    for (; i < count; ++i) sum += x[i] * w[i];
    return sum;
}
void splineStem(const float* x, const float* grid, unsigned rows, unsigned features, float* base, float* spline) {
    for (unsigned r=0;r<rows;++r) for (unsigned f=0;f<features;++f) {
        const float v=x[r*features+f];
        base[r*features+f] = v * (1.0f / (1.0f + std::exp(-v)));
        const float* g=grid+f*13;
        float a[12], b[12];
        for (unsigned j=0;j<12;++j) a[j] = (v>=g[j] && v<g[j+1]) ? 1.0f : 0.0f;
        for (unsigned k=1;k<=3;++k) {
            for (unsigned j=0;j<12-k;++j)
                b[j] = ((v-g[j])/(g[j+k]-g[j]))*a[j] + ((g[j+k+1]-v)/(g[j+k+1]-g[j+1]))*a[j+1];
            std::copy(b,b+12-k,a);
        }
        std::copy(a,a+9,spline+(r*features+f)*9);
    }
}
struct Graph {
    const Weights& weights;
    bool neon;
    std::list<std::vector<float>> storage;
    std::vector<std::function<void()>> operations;
    std::vector<Value> inputs, outputs;
    std::map<std::string,std::pair<Value,Value>> stems;
    std::size_t workspaceBytes=0;
    explicit Graph(const Weights& w,bool vectorized):weights(w),neon(vectorized) {}
    Value buffer(Shape shape) {
        storage.emplace_back(elements(shape)); workspaceBytes += storage.back().size()*sizeof(float);
        return {storage.back().data(),std::move(shape)};
    }
    Value input(Shape shape) { auto v=buffer(std::move(shape));inputs.push_back(v);return v; }
    void output(Value v) { outputs.push_back(v); }
    Value owned(Shape shape,std::vector<float> values) {
        if(elements(shape)!=values.size())throw std::runtime_error("Owned tensor length");
        storage.push_back(std::move(values));workspaceBytes+=storage.back().size()*sizeof(float);
        return {storage.back().data(),std::move(shape)};
    }
    Value scalar(float v) { return owned({1},{v}); }
    Value weight(const std::string& label) {
        return {const_cast<float*>(weights.data(label)),weights.at(label).shape};
    }
    Value reshape(Value x,Shape shape) {
        if(elements(shape)!=size(x))throw std::runtime_error("Reshape length");
        return {x.data,std::move(shape)};
    }
    Value binary(Value a,Value b,char operation) {
        const auto rank=std::max(a.shape.size(),b.shape.size());
        Shape as(rank,1),bs(rank,1),shape(rank);
        std::copy(a.shape.begin(),a.shape.end(),as.end()-a.shape.size());
        std::copy(b.shape.begin(),b.shape.end(),bs.end()-b.shape.size());
        for(unsigned k=0;k<rank;++k) {
            if(as[k]!=bs[k] && as[k]!=1 && bs[k]!=1)throw std::runtime_error("Broadcast mismatch");
            shape[k]=std::max(as[k],bs[k]);
        }
        auto y=buffer(shape);
        if(a.shape==b.shape || size(a)==1 || size(b)==1) {
            const bool scalarA=size(a)==1,scalarB=size(b)==1;
            operations.emplace_back([=](){for(std::size_t n=0;n<size(y);++n){const float x=a.data[scalarA?0:n],z=b.data[scalarB?0:n];y.data[n]=operation=='+'?x+z:operation=='-'?x-z:operation=='*'?x*z:x/z;}});
            return y;
        }
        std::vector<std::size_t> ai(size(y)),bi(size(y));
        for(std::size_t n=0;n<size(y);++n) {
            std::size_t rest=n,sa=1,sb=1;
            for(std::size_t k=rank;k-->0;) {
                const auto c=rest%shape[k];rest/=shape[k];
                if(as[k]!=1)ai[n]+=c*sa;
                if(bs[k]!=1)bi[n]+=c*sb;
                sa*=as[k];sb*=bs[k];
            }
        }
        operations.emplace_back([=]() {
            for(std::size_t n=0;n<size(y);++n) {
                const float x=a.data[ai[n]],z=b.data[bi[n]];
                y.data[n]=operation=='+'?x+z:operation=='-'?x-z:operation=='*'?x*z:x/z;
            }
        });
        return y;
    }
    Value add(Value a,Value b){return binary(a,b,'+');}
    Value sub(Value a,Value b){return binary(a,b,'-');}
    Value mul(Value a,Value b){return binary(a,b,'*');}
    Value div(Value a,Value b){return binary(a,b,'/');}
    Value activation(Value x,armnn::ActivationFunction function) {
        if(function!=armnn::ActivationFunction::ReLu)throw std::runtime_error("Unsupported fused activation");
        auto y=buffer(x.shape);
        operations.emplace_back([=](){for(std::size_t i=0;i<size(x);++i)y.data[i]=std::max(0.0f,x.data[i]);});
        return y;
    }
    Value transpose(Value x,const Shape& permutation) {
        if(permutation.size()!=x.shape.size())throw std::runtime_error("Transpose rank");
        Shape shape(permutation.size());for(unsigned k=0;k<shape.size();++k)shape[k]=x.shape.at(permutation[k]);
        auto y=buffer(shape);
        std::vector<std::size_t> offsets(size(x));
        for(std::size_t n=0;n<size(y);++n) {
            std::size_t rest=n;
            Shape coord(shape.size());for(std::size_t k=shape.size();k-->0;){coord[permutation[k]]=rest%shape[k];rest/=shape[k];}
            std::size_t index=0;for(unsigned k=0;k<coord.size();++k)index=index*x.shape[k]+coord[k];offsets[n]=index;
        }
        operations.emplace_back([=](){for(std::size_t n=0;n<size(y);++n)y.data[n]=x.data[offsets[n]];});
        return y;
    }
    Value slice(Value x,unsigned axis,unsigned start,unsigned length) {
        if(start+length>x.shape.at(axis))throw std::runtime_error("Slice extent");
        auto shape=x.shape;shape[axis]=length;
        std::size_t inner=1,outer=1;
        for(unsigned i=axis+1;i<x.shape.size();++i)inner*=x.shape[i];
        for(unsigned i=0;i<axis;++i)outer*=x.shape[i];
        if(outer==1)return {x.data+start*inner,shape};
        auto y=buffer(shape);
        operations.emplace_back([=](){for(std::size_t i=0;i<outer;++i)std::copy_n(x.data+(i*x.shape[axis]+start)*inner,length*inner,y.data+i*length*inner);});
        return y;
    }
    Value concat(const std::vector<Value>& xs,unsigned axis) {
        auto shape=xs.at(0).shape;shape[axis]=0;
        for(auto x:xs)shape[axis]+=x.shape.at(axis);
        auto y=buffer(shape);
        std::size_t inner=1,outer=1;
        for(unsigned i=axis+1;i<shape.size();++i)inner*=shape[i];
        for(unsigned i=0;i<axis;++i)outer*=shape[i];
        operations.emplace_back([=](){for(std::size_t row=0;row<outer;++row){std::size_t offset=row*shape[axis]*inner;for(const auto& x:xs){const auto count=x.shape[axis]*inner;std::copy_n(x.data+row*count,count,y.data+offset);offset+=count;}}});
        return y;
    }
    Value softmax(Value x) {
        auto y=buffer(x.shape);const auto columns=x.shape.back();const auto rows=size(x)/columns;
        operations.emplace_back([=](){for(std::size_t r=0;r<rows;++r){const float* p=x.data+r*columns;float* q=y.data+r*columns;const float maximum=*std::max_element(p,p+columns);float total=0;for(unsigned c=0;c<columns;++c){q[c]=std::exp(p[c]-maximum);total+=q[c];}for(unsigned c=0;c<columns;++c)q[c]/=total;}});
        return y;
    }
    Value bmm(Value a,Value b) {
        if(a.shape.size()!=3||b.shape.size()!=3||a.shape[0]!=b.shape[0]||a.shape[2]!=b.shape[1])throw std::runtime_error("BMM dimensions");
        const unsigned batch=a.shape[0],rows=a.shape[1],inner=a.shape[2],columns=b.shape[2];
        auto y=buffer({batch,rows,columns});
        operations.emplace_back([=](){for(unsigned n=0;n<batch;++n)for(unsigned i=0;i<rows;++i)for(unsigned j=0;j<columns;++j){float total=0;for(unsigned k=0;k<inner;++k)total+=a.data[(n*rows+i)*inner+k]*b.data[(n*inner+k)*columns+j];y.data[(n*rows+i)*columns+j]=total;}});
        return y;
    }
    Value fc(Value x,const std::string& label) {
        const auto& e=weights.at(label);const unsigned rows=x.shape[0],inner=x.shape[1],units=e.shape[0];
        if(x.shape.size()!=2||e.count!=std::size_t(units)*inner)throw std::runtime_error("Fused FC dimensions");
        const auto selected=quantization.find(label);
        if(selected!=quantization.end()) {
            if(selected->second.inputs!=inner||selected->second.outputs!=units)throw std::runtime_error("Quantized fused dimensions");
            auto kernel=std::make_shared<QuantizedFc>(weights,label,rows,hybridMode,resultDirectory);
            quantizedFcs.push_back(kernel);
            auto y=buffer({rows,units});
            operations.emplace_back([=](){kernel->run(x.data,y.data);});
            return y;
        }
        const float* w=weights.data(label);auto y=buffer({rows,units});const bool vectorized=neon;
        operations.emplace_back([=](){for(unsigned r=0;r<rows;++r)for(unsigned o=0;o<units;++o)y.data[r*units+o]=dot(x.data+r*inner,w+o*inner,inner,vectorized);});
        return y;
    }
    Value kan(Value x,const std::string& prefix) {
        const unsigned features=x.shape.back(),batch=static_cast<unsigned>(size(x)/features);
        const auto& entry=weights.at(prefix+".grid");
        if(entry.shape!=Shape{features,13})throw std::runtime_error("Fused KAN grid");
        const float* grid=weights.data(prefix+".grid");
        std::string key=std::to_string(reinterpret_cast<std::uintptr_t>(x.data));key.append(reinterpret_cast<const char*>(grid),entry.count*sizeof(float));
        Value base,spline;auto it=stems.find(key);
        if(it!=stems.end()){base=it->second.first;spline=it->second.second;}
        else {
            base=buffer({batch,features});spline=buffer({batch,features*9});
            operations.emplace_back([=](){splineStem(x.data,grid,batch,features,base.data,spline.data);});
            stems.emplace(std::move(key),std::make_pair(base,spline));
        }
        auto y=add(fc(base,prefix+".base_weight"),fc(spline,prefix+".spline_weight"));
        auto shape=x.shape;shape.back()=y.shape.back();return reshape(y,std::move(shape));
    }
    Value pcc(Value x) {
        const unsigned rows=x.shape[1],columns=x.shape[2];
        if(x.shape[0]!=1)throw std::runtime_error("PCC batch");
        auto centered=buffer(x.shape),norm=buffer({rows}),y=buffer({1,rows,rows});
        operations.emplace_back([=](){
            for(unsigned r=0;r<rows;++r){float mean=0;for(unsigned c=0;c<columns;++c)mean+=x.data[r*columns+c];mean/=columns;float sq=0;for(unsigned c=0;c<columns;++c){const float v=x.data[r*columns+c]-mean;centered.data[r*columns+c]=v;sq+=v*v;}norm.data[r]=std::sqrt(sq);}
            for(unsigned r=0;r<rows;++r)for(unsigned c=0;c<rows;++c){float sum=0;for(unsigned k=0;k<columns;++k)sum+=centered.data[r*columns+k]*centered.data[c*columns+k];y.data[r*rows+c]=sum/(norm.data[r]*norm.data[c]+1e-8f);}
        });return y;
    }
    Value layerNorm(Value x,const std::string& prefix) {
        const unsigned columns=x.shape.back(),rows=static_cast<unsigned>(size(x)/columns);
        const float* scale=weights.data(prefix+".weight"),*bias=weights.data(prefix+".bias");auto y=buffer(x.shape);
        operations.emplace_back([=](){for(unsigned r=0;r<rows;++r){float mean=0;for(unsigned c=0;c<columns;++c)mean+=x.data[r*columns+c];mean/=columns;float sq=0;for(unsigned c=0;c<columns;++c){const float v=x.data[r*columns+c]-mean;sq+=v*v;}const float norm=std::sqrt(sq/columns+1e-5f);for(unsigned c=0;c<columns;++c)y.data[r*columns+c]=((x.data[r*columns+c]-mean)/norm)*scale[c]+bias[c];}});return y;
    }
    // Architecture methods are generated verbatim from the frozen graph builder.
    std::pair<Value,Value> beforeGelu(Value x,Value adjacency,unsigned block,bool temporal);
    Value afterGelu(Value gelu,Value residual,unsigned block,bool temporal);
    const std::vector<Value>& run(std::initializer_list<View> feeds) {
        if(feeds.size()!=inputs.size())throw std::runtime_error("Fused input count");
        auto feed=feeds.begin();for(const auto& v:inputs){if(feed->size!=size(v))throw std::runtime_error("Fused input extent");std::copy_n(feed->data,feed->size,v.data);++feed;}
        for(auto& execute:operations) execute();
        return outputs;
    }
};
} } // namespace fused, anonymous
