"""FKP-DVGT model architecture."""
import torch
import torch.nn as nn
import math
import torch.nn.functional as F

class KANLayer(nn.Module):

    def __init__(self, in_features, out_features, grid_size=6, spline_order=3, base_activation=nn.SiLU):
        super(KANLayer, self).__init__()
        self.in_features = in_features
        self.out_features = out_features
        self.grid_size = grid_size
        self.spline_order = spline_order
        self.base_weight = nn.Parameter(torch.empty(out_features, in_features))
        self.spline_weight = nn.Parameter(torch.empty(out_features, in_features, grid_size + spline_order))
        grid = torch.linspace(-1, 1, grid_size + 2 * spline_order + 1).repeat(in_features, 1)
        self.register_buffer('grid', grid)
        self.base_activation = base_activation()
        self.reset_parameters()

    def reset_parameters(self):
        nn.init.kaiming_uniform_(self.base_weight, a=math.sqrt(5) * 0.1)
        nn.init.kaiming_uniform_(self.spline_weight, a=math.sqrt(5) * 0.1)

    def b_splines(self, x):
        x = x.unsqueeze(-1)
        grid = self.grid
        bases = ((x >= grid[:, :-1]) & (x < grid[:, 1:])).to(x.dtype)
        for k in range(1, self.spline_order + 1):
            bases = (x - grid[:, :-(k + 1)]) / (grid[:, k:-1] - grid[:, :-(k + 1)]) * bases[:, :, :-1] + (grid[:, k + 1:] - x) / (grid[:, k + 1:] - grid[:, 1:-k]) * bases[:, :, 1:]
        return bases

    def forward(self, x):
        orig_shape = x.shape
        x = x.reshape(-1, self.in_features)
        base_output = F.linear(self.base_activation(x), self.base_weight)
        splines = self.b_splines(x)
        spline_output = torch.einsum('bik,oik->bo', splines, self.spline_weight)
        output = base_output + spline_output
        return output.reshape(*orig_shape[:-1], self.out_features)

def PCC_dist(data):
    mean = data.mean(dim=-1, keepdim=True)
    centered_tensor = data - mean
    dot_product = torch.bmm(centered_tensor, centered_tensor.transpose(1, 2))
    norms = torch.norm(centered_tensor, dim=-1, keepdim=True)
    norms_product = torch.bmm(norms, norms.transpose(1, 2))
    pcc = dot_product / (norms_product + 1e-08)
    return pcc

class TVGTformer(nn.Module):

    def __init__(self, num_nodes, time_length, d_model, num_heads, lambda_param, d_ff, dropout):
        super(TVGTformer, self).__init__()
        self.d_model = d_model
        self.num_heads = num_heads
        self.lambda_param = lambda_param
        self.linears_Q_temp = nn.ModuleList([KANLayer(num_nodes + 1, d_model) for _ in range(self.num_heads)])
        self.linears_K_temp = nn.ModuleList([KANLayer(num_nodes + 1, d_model) for _ in range(self.num_heads)])
        self.linears_V_temp = nn.ModuleList([KANLayer(num_nodes + 1, d_model) for _ in range(self.num_heads)])
        self.W_O_temp = KANLayer(d_model * num_heads, num_nodes + 1)
        self.layer_norm1_temp = nn.LayerNorm(num_nodes + 1)
        self.layer_norm2_temp = nn.LayerNorm(num_nodes + 1)
        self.feed_forward_temp = nn.Sequential(KANLayer(num_nodes + 1, d_ff), nn.GELU(), KANLayer(d_ff, num_nodes + 1))
        self.dropout1 = nn.Dropout(dropout)

    def forward(self, X, A_temp):
        out_temp = []
        for idx in range(self.num_heads):
            Q_temp_i = self.linears_Q_temp[idx](X)
            K_temp_i = self.linears_K_temp[idx](X)
            V_temp_i = self.linears_V_temp[idx](X)
            scores_temp_i = torch.bmm(Q_temp_i, K_temp_i.transpose(-2, -1)) / math.sqrt(self.d_model)
            attention_temp_i = (1 - self.lambda_param) * torch.softmax(scores_temp_i, dim=-1) + self.lambda_param * torch.softmax(nn.ReLU()(A_temp), dim=-1)
            head_v = torch.bmm(torch.softmax(attention_temp_i, -1), V_temp_i)
            out_temp.append(head_v)
        attention_temp = torch.cat(out_temp, -1)
        output_temp = self.W_O_temp(attention_temp)
        output_temp = self.layer_norm1_temp(output_temp) + X
        output_temp = self.dropout1(output_temp)
        ff_output_temp = self.feed_forward_temp(output_temp)
        ff_output_temp = self.layer_norm2_temp(ff_output_temp) + output_temp
        return ff_output_temp

class SVGTformer(nn.Module):

    def __init__(self, num_nodes, time_length, d_model, num_heads, lambda_param, d_ff, dropout):
        super(SVGTformer, self).__init__()
        self.d_model = d_model
        self.num_heads = num_heads
        self.lambda_param = lambda_param
        self.linears_Q_spat = nn.ModuleList([KANLayer(time_length + 1, d_model) for _ in range(self.num_heads)])
        self.linears_K_spat = nn.ModuleList([KANLayer(time_length + 1, d_model) for _ in range(self.num_heads)])
        self.linears_V_spat = nn.ModuleList([KANLayer(time_length + 1, d_model) for _ in range(self.num_heads)])
        self.W_O_spat = KANLayer(d_model * num_heads, time_length + 1)
        self.layer_norm1_spat = nn.LayerNorm(time_length + 1)
        self.layer_norm2_spat = nn.LayerNorm(time_length + 1)
        self.feed_forward_spat = nn.Sequential(KANLayer(time_length + 1, d_ff), nn.GELU(), KANLayer(d_ff, time_length + 1))
        self.dropout2 = nn.Dropout(dropout)

    def forward(self, X, A_spat):
        out_spat = []
        for idx in range(self.num_heads):
            Q_spat_i = self.linears_Q_spat[idx](X)
            K_spat_i = self.linears_K_spat[idx](X)
            V_spat_i = self.linears_V_spat[idx](X)
            scores_spat_i = torch.bmm(Q_spat_i, K_spat_i.transpose(-2, -1)) / math.sqrt(self.d_model)
            attention_spat_i = (1 - self.lambda_param) * torch.softmax(scores_spat_i, dim=-1) + self.lambda_param * torch.softmax(nn.ReLU()(A_spat), dim=-1)
            head_v_spat = torch.bmm(torch.softmax(attention_spat_i, -1), V_spat_i)
            out_spat.append(head_v_spat)
        attention_spat = torch.cat(out_spat, -1)
        output_spat = self.W_O_spat(attention_spat)
        output_spat = self.layer_norm1_spat(output_spat) + X
        ff_output_spat = self.feed_forward_spat(output_spat)
        ff_output_spat = self.layer_norm2_spat(ff_output_spat) + output_spat
        return ff_output_spat

class DVGTformer_model(nn.Module):

    def __init__(self, num_nodes, time_length, d_model, num_heads, lambda_param, d_ff, dropout, num_blocks):
        super(DVGTformer_model, self).__init__()
        if (num_nodes, time_length) != (11, 10):
            raise ValueError("Recovered architecture requires 11 sensors and 10 time steps")
        self.d_model = d_model
        self.num_heads = num_heads
        self.lambda_param = lambda_param
        self.num_blocks = num_blocks
        self.linear_t = KANLayer(110, 110)
        self.linear_x = KANLayer(23, 23)
        self.linear_sample = nn.ModuleList([KANLayer(23, 1) for _ in range(11)])
        self.t_v = nn.Parameter(torch.randn(1, 1, num_nodes))
        self.x_v = nn.Parameter(torch.randn(1, time_length + 1, 1))
        self.fusion = nn.Parameter(torch.full((11, 23), 1 / 23))
        self.register_buffer("positional_encoding", self.create_positional_encoding(time_length + 1, num_nodes + 1), persistent=False)
        self.tvgtformer_blocks = nn.ModuleList([TVGTformer(num_nodes, time_length, d_model[0], num_heads, lambda_param, d_ff[0], dropout) for _ in range(num_blocks)])
        self.svgtformer_blocks = nn.ModuleList([SVGTformer(num_nodes, time_length, d_model[1], num_heads, lambda_param, d_ff[1], dropout) for _ in range(num_blocks)])
        self.output_layer = nn.Sequential(KANLayer((time_length + 1) * (num_nodes + 1), 64), nn.GELU(), KANLayer(64, 1))

    def create_positional_encoding(self, N, d_model):
        pos_encoding = torch.zeros(N, d_model)
        for pos in range(N):
            for i in range(0, d_model - 1, 2):
                pos_encoding[pos, i] = math.sin(pos / 10000 ** (2 * i / d_model))
                pos_encoding[pos, i + 1] = math.cos(pos / 10000 ** (2 * i / d_model))
        return pos_encoding.unsqueeze(0)

    def forward(self, X, graph_probability=0.0):
        if X.ndim != 3 or tuple(X.shape[1:]) != (23, 110):
            raise ValueError("Expected input [batch, 23, 110]")
        bs = X.size(0)
        X = torch.transpose(X, -1, -2)
        X = self.linear_x(X)
        X = X.view(bs, 10, 11, 23)
        bs, T, C, F = X.shape
        assert C == 11 and F == 23
        outs = []
        for j in range(C):
            X_j = X[:, :, j, :]
            X_j = X_j.reshape(bs * T, F)
            Y_j = self.linear_sample[j](X_j)
            Y_j = Y_j.view(bs, T)
            outs.append(Y_j)
        X = torch.stack(outs, dim=2)
        X = torch.cat([X, self.t_v.expand(bs, -1, -1)], dim=1)
        X = torch.cat([X, self.x_v.expand(bs, -1, -1)], dim=-1)
        A_temp = PCC_dist(X)
        A_spat = PCC_dist(X.transpose(-1, -2))
        if graph_probability:
            from .graph import sample_graphs
            A_temp, A_spat = sample_graphs(X, A_temp, A_spat, graph_probability)
        X += self.positional_encoding[:, :X.size(1), :X.size(2)].to(X.device)
        for block_i in range(self.num_blocks):
            X = self.tvgtformer_blocks[block_i](X, A_temp)
            X = torch.transpose(X, 1, 2)
            X = self.svgtformer_blocks[block_i](X, A_spat)
            X = torch.transpose(X, 1, 2)
        X = X.reshape(bs, -1)
        output = self.output_layer(X)
        return output
