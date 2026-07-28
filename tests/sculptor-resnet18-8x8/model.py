import torch
from torch import nn


class BasicBlock(nn.Module):
    expansion = 1

    def __init__(self, input_channels, output_channels, stride=1):
        super().__init__()
        self.conv1 = nn.Conv2d(
            input_channels,
            output_channels,
            kernel_size=3,
            stride=stride,
            padding=1,
            bias=True,
        )
        self.relu = nn.ReLU()
        self.conv2 = nn.Conv2d(
            output_channels,
            output_channels,
            kernel_size=3,
            padding=1,
            bias=True,
        )
        self.projection = (
            nn.Conv2d(
                input_channels,
                output_channels,
                kernel_size=1,
                stride=stride,
                bias=True,
            )
            if stride != 1 or input_channels != output_channels
            else None
        )

    def forward(self, value):
        residual = value
        value = self.relu(self.conv1(value))
        value = self.conv2(value)
        if self.projection is not None:
            residual = self.projection(residual)
        return self.relu(value + residual)


class ResNet18Application(nn.Module):
    def __init__(self, class_count=1000):
        super().__init__()
        torch.manual_seed(0)

        self.stem = nn.Conv2d(
            3,
            64,
            kernel_size=7,
            stride=2,
            padding=3,
            bias=True,
        )
        self.relu = nn.ReLU()
        self.pool = nn.MaxPool2d(kernel_size=3, stride=2, padding=1)
        self.layer1 = self._make_stage(64, 64, block_count=2, stride=1)
        self.layer2 = self._make_stage(64, 128, block_count=2, stride=2)
        self.layer3 = self._make_stage(128, 256, block_count=2, stride=2)
        self.layer4 = self._make_stage(256, 512, block_count=2, stride=2)
        self.average_pool = nn.AdaptiveAvgPool2d((1, 1))
        self.classifier = nn.Linear(512, class_count)

    @staticmethod
    def _make_stage(
        input_channels,
        output_channels,
        block_count,
        stride,
    ):
        blocks = [
            BasicBlock(input_channels, output_channels, stride=stride),
        ]
        blocks.extend(
            BasicBlock(output_channels, output_channels)
            for _ in range(1, block_count)
        )
        return nn.Sequential(*blocks)

    def forward(self, value):
        value = self.pool(self.relu(self.stem(value)))
        value = self.layer1(value)
        value = self.layer2(value)
        value = self.layer3(value)
        value = self.layer4(value)
        value = self.average_pool(value)
        value = torch.flatten(value, 1)
        return self.classifier(value)


SAMPLE_INPUT = torch.linspace(
    -1.0,
    1.0,
    steps=3 * 224 * 224,
    dtype=torch.float32,
).reshape(1, 3, 224, 224)


if __name__ == "__main__":
    model = ResNet18Application().eval()
    result = model(SAMPLE_INPUT)
    print(f"output shape: {tuple(result.shape)}")
    print(f"output sum: {result.sum().item():.9f}")
    print(f"top-1 index: {result.argmax(dim=1).item()}")
